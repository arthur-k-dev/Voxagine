#!/usr/bin/env bash
# Build, install and run the debug APK on a connected device, and follow the
# log that matters.
#
# The engine has no window on Android to print into and no argv to configure
# itself with - SDLActivity passes none - so logcat is the entire diagnostic
# channel. This filters to the tags the engine actually writes plus the two
# Android ones that catch a crash, because unfiltered logcat on a modern phone
# is thousands of lines a second of somebody else's business.
#
# Every adb call here is pinned to one device via -s. Without that, adb
# silently targets "whatever it defaults to" when more than one device or
# emulator is attached - which on this project's own dev machine has meant a
# real phone plugged in alongside a leftover emulator, and every command
# either erroring with "more than one device/emulator" or landing on the
# wrong one. Polling `adb devices` and pinning -s is what makes "it doesn't
# launch on my phone" mean what it says instead of "it launched somewhere
# that isn't your phone."
#
# Every run also deletes Game/Engine/Assets/Shaders/*.spv before building.
# That path is not platform-qualified - it is one shared location every CMake
# preset writes into - so whichever preset was built most recently on this
# machine is silently what the next build packages, and an incremental
# "no work to do" from Ninja does not mean the bytes are right for the
# platform actually being shipped (Docs/MOBILE_PORT_LOG.md's own hazard note).
# Deleting first forces a real recompile, guaranteed fresh for Android.
#
# Every install here is preceded by an uninstall, always - not opt-in - so a
# stale first-launch asset-extraction stamp from a previous APK can never
# survive onto a new one.
#
# Usage:
#   ./run-on-device.sh                 build, install, launch, follow the log
#   ./run-on-device.sh --skip-build    use the APK already on disk
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export PATH="$ANDROID_HOME/platform-tools:$PATH"

package=com.voxagine.bitbuster
activity=$package/.BitBusterActivity

# benchmark, not debug: optimised native code, debug-signed so it installs
# without the user's real release key (build.gradle's own comment - literal
# `release` is unsigned and cannot install at all). Debug is unoptimised C++
# with full symbols, so any number off it is a floor, not a fact.
apk="$here/app/build/outputs/apk/benchmark/app-benchmark.apk"

bSkipBuild=0

for arg in "$@"; do
    case "$arg" in
        --skip-build) bSkipBuild=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

if [ "$bSkipBuild" = "0" ]; then
    echo "== clearing shared shader output, so the build below cannot ship stale ones"
    rm -f "$here"/../../Game/Engine/Assets/Shaders/*.spv

    echo "== building (benchmark)"
    "$here/gradlew" -p "$here" assembleBenchmark
fi

if [ ! -f "$apk" ]; then
    echo "no APK at $apk - build failed, or pass --skip-build only when one already exists" >&2
    exit 1
fi

# Every attached device or emulator, one per line: "<serial>\t<state>". A
# device mid-boot or asleep-and-unauthorized shows up here too, which is
# exactly the case worth telling apart from "not connected at all".
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

    if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt "${#deviceLines[@]}" ]; then
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

# Always, not opt-in: the asset extraction is skipped when its stamp matches,
# so an `install -r` over a previous build can boot straight into stale
# extracted content. Uninstalling first is what guarantees this run is really
# running what was just built.
echo "== uninstalling any previous install from $serial"
adb -s "$serial" uninstall "$package" || true

echo "== device: $serial, $(adb -s "$serial" shell getprop ro.product.model | tr -d '\r') running Android $(adb -s "$serial" shell getprop ro.build.version.release | tr -d '\r')"
echo "== GPU: $(adb -s "$serial" shell dumpsys SurfaceFlinger 2>/dev/null | grep -m1 -i 'GLES:' | tr -d '\r' || echo unknown)"

echo "== installing $(du -h "$apk" | cut -f1) to $serial"
adb -s "$serial" install -r "$apk"

adb -s "$serial" logcat -c
adb -s "$serial" shell am start -n "$activity" >/dev/null

echo "== running on $serial. Ctrl-C to stop."
echo

# SDL routes stdout/stderr to logcat under the tag "SDL/APP", which is where
# every printf in the engine ends up - the [assets], [vulkan], [audio] and
# [fps] lines all arrive there.
adb -s "$serial" logcat -v time \
    SDL:V SDL/APP:V "$package":V \
    AndroidRuntime:E DEBUG:V libc:F \
    '*:S'
