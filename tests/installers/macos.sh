#!/bin/bash
# Run only on an isolated GitHub Actions runner.
set -eu
source_bundle="$PWD/release/obs-textppt.plugin"
parent="$HOME/Library/Application Support/obs-studio/plugins"
target="$parent/obs-textppt.plugin"
[ ! -e "$target" ] || { echo 'The test requires an empty installation path.'; exit 1; }
mkdir -p "$parent"
ditto "$source_bundle" "$target"
printf 'old plugin file\n' > "$target/obsolete.txt"
printf 'preserve\n' > "$parent/installer-test.txt"
mkdir "$parent/obs-textppt.plugin.dSYM"
printf 'old symbols\n' > "$parent/obs-textppt.plugin.dSYM/obsolete.txt"
quarantine='0081;65000000;TextPPT Installer Test;'
for entry in "$source_bundle" "$source_bundle/Contents/MacOS/obs-textppt" "$source_bundle/Contents/Resources/locale/zh-CN.ini" "$parent/installer-test.txt"; do
    /usr/bin/xattr -w com.apple.quarantine "$quarantine" "$entry"
done
/usr/bin/xattr -w com.haliluya.installer-test preserve "$source_bundle"
/bin/bash -n release/install_obs-textppt.sh
start=$(date +%s)
/bin/bash release/install_obs-textppt.sh
[ "$(($(date +%s) - start))" -ge 4 ]
[ ! -e "$target/obsolete.txt" ]
[ ! -e "$parent/obs-textppt.plugin.dSYM" ]
[ "$(cat "$parent/installer-test.txt")" = preserve ]
cmp "$source_bundle/Contents/MacOS/obs-textppt" "$target/Contents/MacOS/obs-textppt"
for language in en-US zh-CN zh-TW; do
    test -f "$target/Contents/Resources/locale/$language.ini"
done
for entry in "$target" "$target/Contents/MacOS/obs-textppt" "$target/Contents/Resources/locale/zh-CN.ini"; do
    if /usr/bin/xattr -p com.apple.quarantine "$entry" >/dev/null 2>&1; then
        echo 'Quarantine remains on the installed plugin.'
        exit 1
    fi
done
[ "$(/usr/bin/xattr -p com.apple.quarantine "$parent/installer-test.txt")" = "$quarantine" ]
[ "$(/usr/bin/xattr -p com.haliluya.installer-test "$target")" = preserve ]
test -x release/install_obs-textppt.sh
if find release -name '*.dSYM' -o -name '*.pkg' | grep .; then
    echo 'Debug symbols and duplicate installers must not be packaged.'
    exit 1
fi
# Exercise real terminal input rather than redirected stdin.
python3 - <<'PY'
import os
import pty
import select
import signal
import time

for key in (b'x', b'\r', b' ', b'\x1b'):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv('/bin/bash', ['bash', 'release/install_obs-textppt.sh'])
    output = b''
    sent = None
    deadline = time.monotonic() + 30
    try:
        while time.monotonic() < deadline:
            if select.select([fd], [], [], 0.02)[0]:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    chunk = b''
                output += chunk
                if sent is None and b'Closing in 5s' in output:
                    os.write(fd, key)
                    sent = time.monotonic()
                if not chunk:
                    break
            if sent is not None and time.monotonic() - sent > 2:
                raise RuntimeError('Any-key countdown exit took too long')
        else:
            raise RuntimeError('Countdown prompt was not reached')
        _, status = os.waitpid(pid, 0)
        assert sent is not None and os.waitstatus_to_exitcode(status) == 0, output.decode(errors='replace')
    finally:
        os.close(fd)
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
print('PASS: macOS letter/Enter/Space/Escape exit preserves the installer result')
PY
echo 'PASS: macOS upgrade, quarantine removal, attribute scope, binary, locales, timeout, and package contents'
