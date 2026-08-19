#!/usr/bin/env bash

# Build, install and run the benchmark APK on a connected device, and follow
# the relevant log output.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export PATH="$ANDROID_HOME/platform-tools:$PATH"

package=com.voxagine.bitbuster
activity="$package/.BitBusterActivity"

# Benchmark: optimised native code, debug-signed so it can be installed
# without the real release key.
apk="$here/app/build/outputs/apk/benchmark/app-benchmark.apk"

bSkipBuild=0

for arg in "$@"; do
    case "$arg" in
        --skip-build)
            bSkipBuild=1
            ;;
        *)
            echo "unknown argument: $arg" >&2
            exit 1
            ;;
    esac
done

if [ "$bSkipBuild" = "0" ]; then
    echo "== clearing shared shader output, so the build cannot ship stale ones"
    rm -f "$here"/../../Game/Engine/Assets/Shaders/*.spv

    echo "== building (benchmark)"
    "$here/gradlew" -p "$here" assembleBenchmark
fi

if [ ! -f "$apk" ]; then
    echo "no APK at $apk - build failed, or pass --skip-build only when one already exists" >&2
    exit 1
fi

# Find attached devices/emulators.
mapfile -t deviceLines < <(adb devices | tail -n +2 | sed '/^$/d')

if [ "${#deviceLines[@]}" -eq 0 ]; then
    echo "no device. On the phone: Settings > About phone > tap Build number" >&2
    echo "seven times, then Developer options > USB debugging, then accept the" >&2
    echo "prompt when you plug it in." >&2
    exit 1
fi

serial=""

if [ "${#deviceLines[@]}" -eq 1 ]; then
    serial="$(echo "${deviceLines[0]}" | awk '{print $1}')"
    state="$(echo "${deviceLines[0]}" | awk '{print $2}')"

    if [ "$state" != "device" ]; then
        echo "only device found ($serial) is in state '$state', not 'device' - check it is unlocked and the USB debugging prompt is accepted" >&2
        exit 1
    fi
else
    echo "== multiple devices attached:"
    i=0

    for line in "${deviceLines[@]}"; do
        i=$((i + 1))
        printf '  %d) %s\n' "$i" "$line"
    done

    read -rp "which one? [1-${#deviceLines[@]}] " choice

    if ! [[ "$choice" =~ ^[0-9]+$ ]] ||
       [ "$choice" -lt 1 ] ||
       [ "$choice" -gt "${#deviceLines[@]}" ]; then
        echo "not a valid choice" >&2
        exit 1
    fi

    serial="$(echo "${deviceLines[$((choice - 1))]}" | awk '{print $1}')"
    state="$(echo "${deviceLines[$((choice - 1))]}" | awk '{print $2}')"

    if [ "$state" != "device" ]; then
        echo "'$serial' is in state '$state', not 'device' - pick one that is actually ready" >&2
        exit 1
    fi
fi

echo "== uninstalling any previous install from $serial"
adb -s "$serial" uninstall "$package" || true

model="$(adb -s "$serial" shell getprop ro.product.model | tr -d '\r')"
android_version="$(adb -s "$serial" shell getprop ro.build.version.release | tr -d '\r')"

echo "== device: $serial, $model running Android $android_version"

gpu="$(
    adb -s "$serial" shell dumpsys SurfaceFlinger 2>/dev/null |
        grep -m1 -i 'GLES:' |
        tr -d '\r' ||
        echo unknown
)"

echo "== GPU: $gpu"

echo "== installing $(du -h "$apk" | cut -f1) to $serial"
adb -s "$serial" install -r "$apk"

adb -s "$serial" logcat -c

echo "== launching $activity"
adb -s "$serial" shell am start -n "$activity" >/dev/null

echo "== running on $serial. Ctrl-C to stop."
echo

# SDL routes stdout/stderr to logcat under SDL/APP.
adb -s "$serial" logcat -v time \
    'SDL:V' \
    'SDL/APP:V' \
    "$package:V" \
    'AndroidRuntime:E' \
    'DEBUG:V' \
    'libc:F' \
    '*:S'
