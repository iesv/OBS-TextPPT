"""Run real OBS checks on a disposable macOS Actions runner."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

if sys.platform != "darwin" or os.environ.get("GITHUB_ACTIONS") != "true":
    raise SystemExit("This runner requires a disposable macOS Actions host")

root = Path(__file__).resolve().parents[2]
output = root / "runtime-reports"
output.mkdir(exist_ok=True)
runtime = Path(os.environ["RUNNER_TEMP"]) / "textppt-obs-runtime"
runtime.mkdir(exist_ok=True)
dmg = runtime / "OBS.dmg"
subprocess.run([
    "curl", "--fail", "--location", "--retry", "3", "--output", str(dmg),
    "https://github.com/obsproject/obs-studio/releases/download/32.0.4/OBS-Studio-32.0.4-macOS-Apple.dmg",
], check=True)
mount = runtime / "mount"
subprocess.run(["hdiutil", "attach", str(dmg), "-nobrowse", "-readonly", "-mountpoint", str(mount)], check=True)
try:
    subprocess.run(["ditto", str(mount / "OBS.app"), str(runtime / "OBS.app")], check=True)
finally:
    subprocess.run(["hdiutil", "detach", str(mount)], check=True)

# The hosted runner's OBS configuration is disposable.
config = Path.home() / "Library/Application Support/obs-studio"
plugins = config / "plugins"
plugins.mkdir(parents=True, exist_ok=True)
probe = next((root / "build_macos/tests").rglob("textppt-ui-probe.plugin"))
for bundle in (root / "release/obs-textppt.plugin", probe):
    subprocess.run(["ditto", str(bundle), str(plugins / bundle.name)], check=True)
    subprocess.run(["xattr", "-dr", "com.apple.quarantine", str(plugins / bundle.name)], check=False)
subprocess.run(["xattr", "-dr", "com.apple.quarantine", str(runtime / "OBS.app")], check=False)
(output / "host.json").write_text(json.dumps({"macOS": platform.mac_ver()[0], "arch": platform.machine()}))


def run(name, extra):
    (config / "basic/profiles/Test").mkdir(parents=True, exist_ok=True)
    (config / "basic/scenes").mkdir(parents=True, exist_ok=True)
    ini = f"""[General]
FirstRun=true
Language={extra.get("TEXTPPT_LANGUAGE", "en-US")}
LastVersion=536870916
MacOSPermissionsDialogLastShown=1

[Basic]
Profile=Test
ProfileDir=Test
SceneCollection=ReleaseCheck
SceneCollectionFile=ReleaseCheck.json

[BasicWindow]
PreviewEnabled=true
StudioMode=false
SceneDuplicationMode={extra.get("TEXTPPT_SCENE_COPIES", "true")}
EditPropertiesMode={extra.get("TEXTPPT_SOURCE_COPIES", "true")}
SwapScenesMode=false
"""
    theme = extra.get("TEXTPPT_THEME", "com.obsproject.Yami.Grey")
    ini += f"\n[Appearance]\nTheme={theme}\n"
    for filename in ("global.ini", "user.ini"):
        (config / filename).write_text(ini)
    (config / "basic/profiles/Test/basic.ini").write_text("""[General]
Name=Test

[Video]
BaseCX=1280
BaseCY=720
OutputCX=1280
OutputCY=720
FPSType=0
FPSCommon=60
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial
""")
    scene = {"name": "ReleaseCheck", "current_scene": "Test", "current_program_scene": "Test",
             "scene_order": [{"name": "Test"}], "sources": [{"name": "Test", "id": "scene",
             "versioned_id": "scene", "settings": {"items": []}, "mixers": 0, "flags": 0, "enabled": True}],
             "groups": [], "transitions": [], "quick_transitions": []}
    (config / "basic/scenes/ReleaseCheck.json").write_text(json.dumps(scene))
    report = output / f"{name}.txt"
    env = os.environ.copy()
    for key in tuple(env):
        if key.startswith("QT_") or key.startswith("DYLD_") or key.startswith("TEXTPPT_"):
            del env[key]
    env.update(extra)
    env["TEXTPPT_UI_REPORT"] = str(report)
    exe = runtime / "OBS.app/Contents/MacOS/OBS"
    complete = "IMAGE TESTS COMPLETE" if "TEXTPPT_IMAGE_TEST" in extra else "UI TESTS COMPLETE"
    with (output / f"{name}.log").open("w") as log:
        process = subprocess.Popen([str(exe), "--multi", "--disable-shutdown-check",
                                   "--disable-missing-files-check", "--disable-updater"],
                                  env=env, stdout=log, stderr=subprocess.STDOUT, cwd=exe.parent)
        try:
            deadline = time.monotonic() + 480
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"OBS exited: {process.returncode}")
                text = report.read_text() if report.exists() else ""
                if not text and time.monotonic() > deadline - 390:
                    raise TimeoutError("OBS did not start the test module within 90 seconds")
                stress = Path(str(report) + ".stress.txt")
                stress_done = "TEXTPPT_STRESS" not in extra or (stress.exists() and "STRESS TEST COMPLETE" in stress.read_text())
                if complete in text and ("FAIL:" in text or stress_done):
                    time.sleep(2)
                    break
                time.sleep(2)
            else:
                raise TimeoutError(f"OBS checks timed out: {name}")
        except TimeoutError:
            # Capture blocked threads before stopping the disposable host.
            try:
                subprocess.run(["/usr/bin/sample", str(process.pid), "3", "1", "-file", str(output / f"{name}.sample.txt")],
                               stdout=log, stderr=subprocess.STDOUT, timeout=10, check=False)
            except (OSError, subprocess.TimeoutExpired) as error:
                print(f"Thread sampling failed: {error}", flush=True)
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    for path in output.glob(f"{name}.txt*"):
        if path.suffix != ".txt":
            continue
        lines = path.read_text().splitlines()
        print(path.name, "passes", sum(line.startswith("PASS:") for line in lines), flush=True)
        if any(line.startswith("FAIL:") for line in lines) or not lines or "COMPLETE" not in lines[-1]:
            raise RuntimeError(f"Failed OBS report: {path.name}")


parser = argparse.ArgumentParser()
parser.add_argument("--suite", choices=("all", "studio", "studio-click", "progressive", "focused", "stress", "layout"), default="all")
suite = parser.parse_args().suite
stress_case = ("ui-and-stress", {"TEXTPPT_STRESS": "1", "TEXTPPT_PROGRESSIVE_ONLY": "1"})
cases = {
    "layout": (
        ("layout-dark", {"TEXTPPT_HEADER_ONLY": "1"}),
        ("layout-light", {"TEXTPPT_HEADER_ONLY": "1", "TEXTPPT_THEME": "com.obsproject.Yami.Light"}),
        ("layout-zh-CN", {"TEXTPPT_HEADER_ONLY": "1", "TEXTPPT_LANGUAGE": "zh-CN"}),
        ("layout-zh-TW", {"TEXTPPT_HEADER_ONLY": "1", "TEXTPPT_LANGUAGE": "zh-TW"}),
    ),
    "stress": (stress_case,),
    "focused": (
        ("studio", {"TEXTPPT_STUDIO_ONLY": "1"}),
        ("progressive", {"TEXTPPT_PROGRESSIVE_ONLY": "1"}),
    ),
    "studio": (("studio", {"TEXTPPT_STUDIO_ONLY": "1"}),),
    "studio-click": (
        ("click-groups", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_DOCK_DOUBLE_CLICK": "1", "TEXTPPT_DOCK_PREVIEW": "1", "TEXTPPT_STUDIO_STRESS": "1"}),
        ("click-group-refs", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_DOCK_DOUBLE_CLICK": "1", "TEXTPPT_DOCK_PREVIEW": "1", "TEXTPPT_SOURCE_COPIES": "false"}),
        ("click-group-shared", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_DOCK_DOUBLE_CLICK": "1", "TEXTPPT_DOCK_PREVIEW": "1", "TEXTPPT_SCENE_COPIES": "false", "TEXTPPT_SOURCE_COPIES": "false"}),
        ("click-text", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_DOCK_DOUBLE_CLICK": "1", "TEXTPPT_DOCK_PREVIEW": "1", "TEXTPPT_NATIVE_TEXT": "1", "TEXTPPT_STUDIO_STRESS": "1"}),
        ("click-text-refs", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_DOCK_DOUBLE_CLICK": "1", "TEXTPPT_DOCK_PREVIEW": "1", "TEXTPPT_NATIVE_TEXT": "1", "TEXTPPT_SOURCE_COPIES": "false"}),
        ("click-text-shared", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_DOCK_DOUBLE_CLICK": "1", "TEXTPPT_DOCK_PREVIEW": "1", "TEXTPPT_NATIVE_TEXT": "1", "TEXTPPT_SCENE_COPIES": "false", "TEXTPPT_SOURCE_COPIES": "false"}),
        ("studio", {"TEXTPPT_STUDIO_ONLY": "1"}),
        ("ui-dark", {}),
        ("ui-light", {"TEXTPPT_THEME": "com.obsproject.Yami.Light"}),
    ),
    "progressive": (("progressive", {"TEXTPPT_PROGRESSIVE_ONLY": "1"}),),
    "all": (
        ("click-groups", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_DOCK_DOUBLE_CLICK": "1", "TEXTPPT_DOCK_PREVIEW": "1"}),
        ("click-text", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_DOCK_DOUBLE_CLICK": "1", "TEXTPPT_DOCK_PREVIEW": "1", "TEXTPPT_NATIVE_TEXT": "1"}),
        ("studio-copies", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_STUDIO_STRESS": "1"}),
        ("studio-scene-copy", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_STUDIO_STRESS": "1", "TEXTPPT_SOURCE_COPIES": "false"}),
        ("studio-shared", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_STUDIO_STRESS": "1", "TEXTPPT_SCENE_COPIES": "false", "TEXTPPT_SOURCE_COPIES": "false"}),
        ("studio-text-copies", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_NATIVE_TEXT": "1", "TEXTPPT_STUDIO_STRESS": "1"}),
        ("studio-text-scene-copy", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_NATIVE_TEXT": "1", "TEXTPPT_SOURCE_COPIES": "false"}),
        ("studio-text-shared", {"TEXTPPT_NATIVE_STUDIO_ONLY": "1", "TEXTPPT_NATIVE_TEXT": "1", "TEXTPPT_SCENE_COPIES": "false", "TEXTPPT_SOURCE_COPIES": "false"}),
        ("chinese", {"TEXTPPT_CJK_ONLY": "1", "TEXTPPT_PROGRESSIVE_ONLY": "1"}),
        stress_case,
        ("ui-dark", {}),
        ("images", {"TEXTPPT_IMAGE_TEST": "1", "TEXTPPT_IMAGE_STRESS": "1"}),
        ("ui-light", {"TEXTPPT_THEME": "com.obsproject.Yami.Light"}),
    ),
}
failures = []
try:
    for name, options in cases[suite]:
        try:
            run(name, options)
        except (RuntimeError, TimeoutError) as error:
            failures.append(f"{name}: {error}")
            print(failures[-1], flush=True)
finally:
    logs = config / "logs"
    if logs.exists():
        shutil.copytree(logs, output / "obs-logs", dirs_exist_ok=True)
if failures:
    raise SystemExit("\n".join(failures))
