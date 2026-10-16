#!/bin/bash
# Install only this plugin bundle. OBS settings remain in their own directory.
set -eu

staging_dir=""
plugin_target=""
installed=0

finish() {
    result=$?
    trap - EXIT
    if [ -n "$staging_dir" ] && [ -d "$staging_dir" ]; then
        if [ "$installed" -eq 0 ] && [ -d "$staging_dir/previous.plugin" ]; then
            if [ ! -e "$plugin_target" ] && mv "$staging_dir/previous.plugin" "$plugin_target"; then
                printf 'Previous version restored.\n'
            else
                printf 'Recovery copy retained at: %s\n' "$staging_dir" >&2
            fi
        fi
        if [ ! -e "$staging_dir/previous.plugin" ] || [ "$installed" -eq 1 ]; then
            rm -rf -- "$staging_dir" || printf 'Temporary files retained at: %s\n' "$staging_dir" >&2
        fi
    fi
    # Close after five seconds or any terminal key.
    for seconds in 5 4 3 2 1; do
        printf '\rClosing in %ss... Press any key to exit. ' "$seconds"
        if [ -t 0 ]; then
            if IFS= read -r -s -n 1 -t 1; then break; fi
        else
            sleep 1
        fi
    done
    printf '\n'
    exit "$result"
}

fail() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'OBS-TextPPT 5.0.0 - macOS arm64 / OBS 32\n'
[ "$(uname -s)" = Darwin ] || fail 'This script is for macOS.'
[ "$(uname -m)" = arm64 ] || fail 'Run in an Apple Silicon arm64 terminal.'
[ "$(id -u)" -ne 0 ] || fail 'Run without sudo; the plugin is installed for your user.'
if pgrep -x OBS >/dev/null; then
    fail 'Close all OBS windows before installing.'
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
plugin_source="$script_dir/obs-textppt.plugin"
[ -d "$plugin_source" ] && [ ! -L "$plugin_source" ] || fail 'Extract the complete ZIP first.'
[ -f "$plugin_source/Contents/Info.plist" ] || fail 'The plugin bundle is incomplete.'
[ -f "$plugin_source/Contents/MacOS/obs-textppt" ] || fail 'The plugin binary is missing.'
for language in en-US zh-CN zh-TW; do
    [ -f "$plugin_source/Contents/Resources/locale/$language.ini" ] || fail 'Language files are missing.'
done
# Check the thin arm64 header using system tools; Xcode is not required.
mach_header=$(/usr/bin/od -An -tx1 -N8 "$plugin_source/Contents/MacOS/obs-textppt" | /usr/bin/tr -d '[:space:]')
[ "$mach_header" = cffaedfe0c000001 ] || fail 'The plugin must contain only arm64 code.'

plugin_parent="$HOME/Library/Application Support/obs-studio/plugins"
mkdir -p "$plugin_parent"
plugin_parent=$(CDPATH= cd -- "$plugin_parent" && pwd -P)
plugin_target="$plugin_parent/obs-textppt.plugin"
[ ! -L "$plugin_target" ] || fail 'The installed plugin must not be a symbolic link.'
[ ! -e "$plugin_target" ] || [ -d "$plugin_target" ] || fail 'The installation path is not a directory.'
case "$plugin_source/" in
    "$plugin_target/"*) fail 'Extract the package outside the installation folder.' ;;
esac

# Stage on the same volume before replacing the existing bundle.
staging_dir=$(mktemp -d "$plugin_parent/.obs-textppt-install.XXXXXXXX")
/usr/bin/ditto "$plugin_source" "$staging_dir/new.plugin"
# Clear only this bundle's quarantine before replacing the installed plugin.
/usr/bin/xattr -drs com.apple.quarantine "$staging_dir/new.plugin" || fail 'Cannot clear the plugin quarantine attribute.'
if [ -e "$plugin_target" ]; then
    mv "$plugin_target" "$staging_dir/previous.plugin"
fi
mv "$staging_dir/new.plugin" "$plugin_target"
installed=1
if [ -d "$plugin_parent/obs-textppt.plugin.dSYM" ] && [ ! -L "$plugin_parent/obs-textppt.plugin.dSYM" ]; then
    mv "$plugin_parent/obs-textppt.plugin.dSYM" "$staging_dir/previous-symbols"
fi
printf 'Installed successfully: %s\nRestart OBS and open Docks > TextPPT.\n' "$plugin_target"
