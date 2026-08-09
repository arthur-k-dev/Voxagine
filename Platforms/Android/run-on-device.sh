#!/usr/bin/env bash
# Install and run the debug APK on whatever adb can see, and capture the log
# that matters.
#
# The engine has no window on Android to print into and no argv to configure
# itself with - SDLActivity passes none - so logcat is the entire diagnostic
# channel. This filters to the tags the engine actually writes plus the two
# Android ones that catch a crash, because unfiltered logcat on a modern phone
# is thousands of lines a second of somebody else's business.
#
# Usage:
#   ./run-on-device.sh                 install, launch, follow the log
#   ./run-on-device.sh --reinstall     uninstall first, so first-launch asset
#                                      extraction runs again from scratch
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export PATH="$ANDROID_HOME/platform-tools:$PATH"

package=com.voxagine.bitbuster
activity=$package/.BitBusterActivity
apk="$here/app/build/outputs/apk/debug/app-debug.apk"

if [ ! -f "$apk" ]; then
    echo "no APK at $apk - run ./gradlew assembleDebug first" >&2
    exit 1
fi

if ! adb get-state >/dev/null 2>&1; then
    echo "no device. On the phone: Settings > About phone > tap Build number" >&2
    echo "seven times, then Developer options > USB debugging, then accept the" >&2
    echo "prompt when you plug it in." >&2
    exit 1
fi

if [ "${1:-}" = "--reinstall" ]; then
    # The asset extraction is skipped when the stamp matches, so re-testing it
    # means removing the app data, not just overwriting the APK.
    echo "== uninstalling, so first-launch extraction runs again"
    adb uninstall "$package" || true
fi

echo "== device: $(adb shell getprop ro.product.model | tr -d '\r') running Android $(adb shell getprop ro.build.version.release | tr -d '\r')"
echo "== GPU: $(adb shell dumpsys SurfaceFlinger 2>/dev/null | grep -m1 -i 'GLES:' | tr -d '\r' || echo unknown)"

echo "== installing $(du -h "$apk" | cut -f1)"
adb install -r "$apk"

adb logcat -c
adb shell am start -n "$activity" >/dev/null

echo "== running. Ctrl-C to stop."
echo

# SDL routes stdout/stderr to logcat under the tag "SDL/APP", which is where
# every printf in the engine ends up - the [assets], [vulkan], [audio] and
# [fps] lines all arrive there.
adb logcat -v time \
    SDL:V SDL/APP:V "$package":V \
    AndroidRuntime:E DEBUG:V libc:F \
    '*:S'
