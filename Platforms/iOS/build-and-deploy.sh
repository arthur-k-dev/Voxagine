#!/bin/sh
# Build Bit Buster or the Voxagine editor for iOS, then either package a
# valid IPA and publish it to the SideStore source hosted on the NUC, or
# install it straight onto an attached, paired device. Linux cannot compile
# iOS binaries, but can run this script with --skip-build against an existing
# .app bundle (SideStore route only - a device install needs a real Apple
# signature, which only Xcode on macOS can produce).
#
# The two are separate applications with separate bundle identifiers, so they
# install side by side on a device and have separate entries in source.json.
# Everything that differs between them is in the case statement below; nothing
# further down names one or the other.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
NUC=${NUC:-joey@192.168.2.5}
NUC_DIR=${NUC_DIR:-/home/joey/altserver}
SOURCE_PATH=${SOURCE_PATH:-"$NUC_DIR/store/source.json"}
SOURCE_NAME=${SOURCE_NAME:-NUC}
VERSION=${VERSION:-0.1.$(date +%Y%m%d)}
SSH_KEY=${SSH_KEY:-}
SKIP_BUILD=0
TARGET_EXPLICIT=0
[ -n "${TARGET:-}" ] && TARGET_EXPLICIT=1
TARGET=${TARGET:-game}
DEST_EXPLICIT=0
[ -n "${DEST:-}" ] && DEST_EXPLICIT=1
DEST=${DEST:-sidestore}
CONFIG_EXPLICIT=0
[ -n "${CONFIG:-}" ] && CONFIG_EXPLICIT=1
CONFIG=${CONFIG:-Release}
DEVICE_ID=${DEVICE_ID:-}
IOS_DEVELOPMENT_TEAM=${IOS_DEVELOPMENT_TEAM:-}

# SideStore compares the IPA's Info.plist as well as its source metadata. A
# changing source.json version is not enough when CFBundleVersion remains 1:
# iOS then keeps the previously installed executable. Derive an Apple-valid
# three-component marketing version and a monotonically increasing build
# number from the source version (e.g. 0.1.20260811.7 -> 0.20260811.7,
# 202608117). Both may be explicitly overridden for release builds.
VERSION_DATE=$(printf '%s' "$VERSION" | awk -F. 'NF >= 4 { print $(NF - 1) }')
VERSION_REVISION=$(printf '%s' "$VERSION" | awk -F. 'NF >= 4 { print $NF }')
if [ -n "$VERSION_DATE" ] && [ -n "$VERSION_REVISION" ]; then
    IOS_BUNDLE_VERSION=${IOS_BUNDLE_VERSION:-"0.$VERSION_DATE.$VERSION_REVISION"}
    IOS_BUILD_NUMBER=${IOS_BUILD_NUMBER:-"$VERSION_DATE$VERSION_REVISION"}
else
    IOS_BUNDLE_VERSION=${IOS_BUNDLE_VERSION:-"$VERSION"}
    IOS_BUILD_NUMBER=${IOS_BUILD_NUMBER:-"$(date +%Y%m%d%H%M%S)"}
fi
# Keep SideStore's source metadata identical to the IPA marketing version.
# SideStore rejects an update when apps[].version and
# CFBundleShortVersionString describe different releases.
SOURCE_VERSION=${SOURCE_VERSION:-"$IOS_BUNDLE_VERSION"}

# When SSH_KEY is empty these wrappers deliberately call plain ssh/scp, which
# preserves the normal interactive password prompt in a terminal.
scp_to_nuc() {
    if [ -n "$SSH_KEY" ]; then scp -i "$SSH_KEY" "$@"; else scp "$@"; fi
}

ssh_nuc() {
    if [ -n "$SSH_KEY" ]; then ssh -i "$SSH_KEY" "$@"; else ssh "$@"; fi
}

usage() {
    echo "Usage: $0 [--game|--editor] [--sidestore|--device] [--release|--debug] [--skip-build]"
    echo "  --game       Bit Buster, com.voxagine.bitbuster (default)"
    echo "  --editor     the Voxagine editor, com.voxagine.bitbuster.editor"
    echo
    echo "  --sidestore  package an IPA and publish it to the NUC's SideStore"
    echo "               source; unsigned, since SideStore re-signs on install"
    echo "               (default)"
    echo "  --device     build signed and install straight onto a paired"
    echo "               device via devicectl. Picks the device and Apple"
    echo "               Development signing team interactively when there's"
    echo "               a choice (or a terminal to ask); set DEVICE_ID /"
    echo "               IOS_DEVELOPMENT_TEAM to skip the prompts"
    echo
    echo "  --release    optimised, no Vulkan validation layers (default)"
    echo "  --debug      unoptimised, with Vulkan validation layers"
    echo
    echo "  macOS: configure/build the chosen app, then package and publish it"
    echo "         or install it, per --sidestore/--device"
    echo "  Linux: use --skip-build with an existing .app bundle (--sidestore"
    echo "         only - --device needs Xcode to produce a real signature)"
    echo
    echo "  Each app needs its own entry in source.json, matched on"
    echo "  bundleIdentifier. Publishing refuses rather than guessing if the"
    echo "  chosen app has no entry there yet."
}

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
        --game) TARGET=game; TARGET_EXPLICIT=1 ;;
        --editor) TARGET=editor; TARGET_EXPLICIT=1 ;;
        --sidestore) DEST=sidestore; DEST_EXPLICIT=1 ;;
        --device) DEST=device; DEST_EXPLICIT=1 ;;
        --release) CONFIG=Release; CONFIG_EXPLICIT=1 ;;
        --debug) CONFIG=Debug; CONFIG_EXPLICIT=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

# Neither --game/--editor nor $TARGET was given. Ask rather than silently
# building the game when a real terminal is attached; a non-interactive
# caller (CI, a script) gets the documented game default instead of hanging
# on a prompt with no one to answer it.
if [ "$TARGET_EXPLICIT" -eq 0 ] && [ -t 0 ]; then
    printf 'Deploy which app? [g]ame (default) / [e]ditor: '
    read -r REPLY
    case "$REPLY" in
        e|E|editor) TARGET=editor ;;
        *) TARGET=game ;;
    esac
fi

# Same idea for the destination: don't silently pick one when a person is
# sitting at the terminal and neither --sidestore/--device nor $DEST said so.
if [ "$DEST_EXPLICIT" -eq 0 ] && [ -t 0 ]; then
    printf 'Deploy where? [s]ideStore (default) / [d]evice: '
    read -r REPLY
    case "$REPLY" in
        d|D|device) DEST=device ;;
        *) DEST=sidestore ;;
    esac
fi

# Same idea for the build configuration.
if [ "$CONFIG_EXPLICIT" -eq 0 ] && [ -t 0 ]; then
    printf 'Which build? [r]elease (default) / [d]ebug: '
    read -r REPLY
    case "$REPLY" in
        d|D|debug) CONFIG=Debug ;;
        *) CONFIG=Release ;;
    esac
fi

case "$CONFIG" in
    Release|Debug) ;;
    *) echo "CONFIG must be 'Release' or 'Debug', got '$CONFIG'" >&2; exit 2 ;;
esac

case "$DEST" in
    sidestore|device) ;;
    *) echo "DEST must be 'sidestore' or 'device', got '$DEST'" >&2; exit 2 ;;
esac

# --device installs a real Apple signature, which only devicectl (Xcode 15+,
# macOS-only) can consume; there is no equivalent tool on Linux.
if [ "$DEST" = device ] && [ "$(uname -s)" != Darwin ]; then
    echo "--device needs macOS: devicectl, which installs onto the device, is Apple's own tool." >&2
    exit 2
fi

# Resolve *which* device and *which* signing team up front, before spending
# minutes on a build - both are knowable without one, and failing here beats
# failing on the codesign step afterwards with a build already sitting there.
if [ "$DEST" = device ]; then
    DEVICE_TMP=$(mktemp -d "${TMPDIR:-/tmp}/voxagine-device.XXXXXX")
    trap 'rm -rf "$DEVICE_TMP"' EXIT INT TERM

    if [ -z "$DEVICE_ID" ]; then
        DEVICES_JSON="$DEVICE_TMP/devices.json"
        xcrun devicectl list devices --json-output "$DEVICES_JSON" >/dev/null
        DEVICE_LIST=$(python3 -c '
import json, sys
with open(sys.argv[1]) as f:
    data = json.load(f)
for d in data.get("result", {}).get("devices", []):
    props = d.get("connectionProperties", {}) or {}
    if props.get("pairingState") == "paired" and props.get("tunnelState") in ("connected", "disconnected"):
        print(d["identifier"] + "\t" + d.get("deviceProperties", {}).get("name", "?"))
' "$DEVICES_JSON")
        DEVICE_COUNT=$(printf '%s\n' "$DEVICE_LIST" | grep -c . || true)
        if [ "$DEVICE_COUNT" -eq 0 ]; then
            echo "No paired iOS device found. Pair one in Xcode (Window > Devices and Simulators), or set DEVICE_ID." >&2
            exit 1
        fi
        echo "Paired devices:"
        i=0
        printf '%s\n' "$DEVICE_LIST" | while IFS='	' read -r id name; do
            i=$((i + 1))
            echo "  $i) $name ($id)"
        done
        if [ "$DEVICE_COUNT" -eq 1 ]; then
            DEVICE_ID=$(printf '%s\n' "$DEVICE_LIST" | cut -f1)
            echo "Only one, so installing on that."
        elif [ -t 0 ]; then
            printf 'Install on which? [1-%s]: ' "$DEVICE_COUNT"
            read -r PICK
            DEVICE_ID=$(printf '%s\n' "$DEVICE_LIST" | sed -n "${PICK}p" | cut -f1)
            if [ -z "$DEVICE_ID" ]; then
                echo "Invalid selection: $PICK" >&2
                exit 1
            fi
        else
            echo "Multiple paired devices found and no terminal to ask interactively; set DEVICE_ID." >&2
            exit 1
        fi
    fi

    if [ -z "$IOS_DEVELOPMENT_TEAM" ]; then
        # Xcode's own account cache, not Keychain: a Keychain "Apple
        # Development" certificate can outlive the Xcode account it was
        # issued under (left over from a reinstalled Xcode, a different
        # Apple ID, an expired membership, ...), and automatic signing then
        # fails with "No Account for Team" even though the cert looks fine.
        # The teams Xcode lists here are the ones it can actually fetch a
        # profile for.
        TEAM_LIST=$(python3 -c '
import plistlib, os, sys
path = os.path.expanduser("~/Library/Preferences/com.apple.dt.Xcode.plist")
try:
    with open(path, "rb") as f:
        data = plistlib.load(f)
except (FileNotFoundError, plistlib.InvalidFileException):
    sys.exit(0)
seen = set()
for account, teams in (data.get("IDEProvisioningTeams") or {}).items():
    for t in teams or []:
        team_id = t.get("teamID")
        if not team_id or team_id in seen:
            continue
        seen.add(team_id)
        print(team_id + "\t" + t.get("teamName", "?") + " (" + account + ")")
' 2>/dev/null)
        TEAM_COUNT=$(printf '%s\n' "$TEAM_LIST" | grep -c . || true)
        if [ "$TEAM_COUNT" -eq 1 ]; then
            IOS_DEVELOPMENT_TEAM=$(printf '%s\n' "$TEAM_LIST" | cut -f1)
            echo "Signing team: $(printf '%s\n' "$TEAM_LIST" | cut -f2) ($IOS_DEVELOPMENT_TEAM)"
        elif [ "$TEAM_COUNT" -gt 1 ] && [ -t 0 ]; then
            echo "Signing teams found in Xcode:"
            i=0
            printf '%s\n' "$TEAM_LIST" | while IFS='	' read -r id name; do
                i=$((i + 1))
                echo "  $i) $name ($id)"
            done
            printf 'Sign with which team? [1-%s]: ' "$TEAM_COUNT"
            read -r PICK
            IOS_DEVELOPMENT_TEAM=$(printf '%s\n' "$TEAM_LIST" | sed -n "${PICK}p" | cut -f1)
            if [ -z "$IOS_DEVELOPMENT_TEAM" ]; then
                echo "Invalid selection: $PICK" >&2
                exit 1
            fi
        elif [ -t 0 ]; then
            echo "No signing team found in Xcode's account cache. Sign in via" >&2
            echo "Xcode > Settings > Accounts first, or enter one manually." >&2
            printf 'Apple team ID (10 characters): '
            read -r IOS_DEVELOPMENT_TEAM
        fi
        if [ -z "$IOS_DEVELOPMENT_TEAM" ]; then
            echo "--device needs IOS_DEVELOPMENT_TEAM set to your 10-character Apple team ID." >&2
            echo "(Xcode > Settings > Accounts lists the teams your signed-in Apple ID belongs to.)" >&2
            exit 2
        fi
    fi
fi

# Everything that differs between the two applications. Set after the option
# parsing so --editor can pick the defaults, but each is still overridable from
# the environment.
case "$TARGET" in
    game)
        CMAKE_TARGET=BitBuster
        BUILD_EDITOR=OFF
        APP_EXECUTABLE=BitBuster
        BUNDLE_ID=com.voxagine.bitbuster
        DEFAULT_BUILD_ROOT="$ROOT/Build/iOS/Game"
        DEFAULT_IPA_NAME=Voxagine.ipa
        DEFAULT_APP_NAME="Bit Buster"
        ;;
    editor)
        CMAKE_TARGET=VoxagineEditor
        BUILD_EDITOR=ON
        APP_EXECUTABLE=Voxagine
        BUNDLE_ID=com.voxagine.bitbuster.editor
        DEFAULT_BUILD_ROOT="$ROOT/Build/iOS/Editor"
        DEFAULT_IPA_NAME=VoxagineEditor.ipa
        DEFAULT_APP_NAME="Voxagine Editor"
        ;;
    *)
        echo "TARGET must be 'game' or 'editor', got '$TARGET'" >&2
        exit 2
        ;;
esac

# A device build needs the Xcode generator, for the XCODE_ATTRIBUTE_* signing
# properties in CMakeLists.txt to take effect - the Makefile generator used
# for --sidestore silently ignores them and links, unsigned, into ".../Make".
# Different generator means a different cache, so it gets its own tree rather
# than reconfiguring "Make" in place, which CMake refuses to do anyway.
case "$DEST" in
    device) DEFAULT_BUILD_DIR="$DEFAULT_BUILD_ROOT/Xcode" ;;
    *) DEFAULT_BUILD_DIR="$DEFAULT_BUILD_ROOT/Make" ;;
esac

BUILD_DIR=${BUILD_DIR:-"$DEFAULT_BUILD_DIR"}
APP_EXPLICIT=0
[ -n "${APP:-}" ] && APP_EXPLICIT=1
APP=${APP:-"$BUILD_DIR/bin/$APP_EXECUTABLE.app"}
IPA_NAME=${IPA_NAME:-"$DEFAULT_IPA_NAME"}
APP_NAME=${APP_NAME:-"$DEFAULT_APP_NAME"}

if [ "$SKIP_BUILD" -eq 0 ]; then
    case "$(uname -s)" in
        Darwin)
            if [ "$DEST" = device ]; then
                cmake -S "$ROOT" -B "$BUILD_DIR" \
                    -G Xcode \
                    -DCMAKE_SYSTEM_NAME=iOS \
                    -DCMAKE_OSX_ARCHITECTURES=arm64 \
                    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
                    -DVOXAGINE_BUILD_ENGINE=ON \
                    -DVOXAGINE_BUILD_EDITOR=$BUILD_EDITOR \
                    -DVOXAGINE_BUILD_BRINGUP=OFF \
                    -DVOXAGINE_ASSET_VERSION="$IOS_BUNDLE_VERSION" \
                    -DVOXAGINE_IOS_DEVELOPMENT_TEAM="$IOS_DEVELOPMENT_TEAM" \
                    -DCMAKE_BUILD_TYPE=$CONFIG
                # -allowProvisioningUpdates lets Xcode create/download the
                # provisioning profile for this bundle ID on demand instead of
                # failing the build the first time a new app ID is signed.
                cmake --build "$BUILD_DIR" --config "$CONFIG" --target "$CMAKE_TARGET" -- -allowProvisioningUpdates
            else
                cmake -S "$ROOT" -B "$BUILD_DIR" \
                    -DCMAKE_SYSTEM_NAME=iOS \
                    -DCMAKE_OSX_ARCHITECTURES=arm64 \
                    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
                    -DVOXAGINE_BUILD_ENGINE=ON \
                    -DVOXAGINE_BUILD_EDITOR=$BUILD_EDITOR \
                    -DVOXAGINE_BUILD_BRINGUP=OFF \
                    -DVOXAGINE_ASSET_VERSION="$IOS_BUNDLE_VERSION" \
                    -DCMAKE_BUILD_TYPE=$CONFIG
                cmake --build "$BUILD_DIR" --config "$CONFIG" --target "$CMAKE_TARGET"
            fi
            ;;
        *)
            echo "iOS compilation requires macOS/Xcode. On Linux use --skip-build." >&2
            exit 2
            ;;
    esac
fi

# The Xcode generator nests the product under a Configuration(-Platform)
# subdirectory that varies by Xcode version, rather than the flat "bin/" that
# the single-config Makefile generator produces - so for --device, fall back
# to a search instead of guessing that layout. An explicit $APP is trusted
# as-is either way. Prefer a match whose path names the config actually just
# built, in case both a Debug and Release .app exist side by side.
if [ "$APP_EXPLICIT" -eq 0 ] && [ "$DEST" = device ] && [ ! -d "$APP" ]; then
    FOUND_APP=$(find "$BUILD_DIR" -type d -path "*$CONFIG*/$APP_EXECUTABLE.app" -print -quit 2>/dev/null)
    [ -z "$FOUND_APP" ] && FOUND_APP=$(find "$BUILD_DIR" -type d -name "$APP_EXECUTABLE.app" -print -quit 2>/dev/null)
    [ -n "$FOUND_APP" ] && APP="$FOUND_APP"
fi

if [ ! -d "$APP" ] || [ ! -f "$APP/Info.plist" ] || [ ! -x "$APP/$APP_EXECUTABLE" ]; then
    echo "$APP_EXECUTABLE.app is missing or incomplete: $APP" >&2
    exit 1
fi

# Shader compilation writes generated .spv files beside the source .hlsl
# files. CMake's bundle copy is POST_BUILD on the app target, so it does not
# rerun when only a shader changed and the executable did not need relinking.
# Always resync the content tree before inspecting or packaging the bundle;
# cmake -E is deliberately used here because this path also runs on Linux.
#
# The same two directories and two loose files that voxagine_bundle_assets()
# ships, and for the same reason - this used to copy all of Game/, which put
# the desktop working directory into the IPA: the game's own Source/ tree, both
# Optick DLLs, the Visual Studio .rc files, and SmallHouseV2. Keep this list in
# step with the one in CMakeLists.txt and with MobileAssets' k_pAssetRoots.
if [ -d "$ROOT/Game" ]; then
    cmake -E copy_directory "$ROOT/Game/Content" "$APP/Content"
    cmake -E copy_directory "$ROOT/Game/Engine" "$APP/Engine"
    cmake -E copy_if_different \
        "$ROOT/Game/Settings.vgs" "$ROOT/Game/ProjectSettings.vgps" "$APP"
fi

# A Vulkan executable can launch perfectly while rendering nothing if its
# generated SPIR-V assets were skipped (for example when dxc/glslc was absent
# during CMake configuration). Fail the deployment before it replaces the
# working IPA instead of making that a black-screen surprise on the device.
SHADER_DIR="$APP/Engine/Assets/Shaders"
if [ -d "$SHADER_DIR" ]; then
    SHADER_SOURCES=$(find "$SHADER_DIR" -type f \( -name '*.vs.hlsl' -o -name '*.ps.hlsl' -o -name '*.cs.hlsl' \) | wc -l | tr -d ' ')
    SHADER_MODULES=$(find "$SHADER_DIR" -type f -name '*.spv' | wc -l | tr -d ' ')
    if [ "$SHADER_SOURCES" -gt 0 ] && [ "$SHADER_MODULES" -lt "$SHADER_SOURCES" ]; then
        echo "Refusing to package $APP: found $SHADER_MODULES SPIR-V modules for $SHADER_SOURCES shader stages." >&2
        exit 1
    fi
fi

# CMake's bundle copy can pick up a temporary linker executable. It is not an
# app resource and makes the IPA unnecessarily huge.
find "$APP" -type f -name "$APP_EXECUTABLE.ld_*" -delete

if [ "$DEST" = device ]; then
    # The content sync above runs unconditionally (it also covers the
    # shader-only-change gap noted above) and can touch files inside an
    # already-signed bundle, which invalidates Xcode's signature. Capture the
    # entitlements Xcode embedded before that happens and re-sign with them so
    # devicectl's install still matches the provisioning profile it built
    # against - a fresh "automatic" resolve here could pick a different
    # identity/profile than the one -allowProvisioningUpdates just fetched.
    ENTITLEMENTS="$DEVICE_TMP/entitlements.plist"
    if ! codesign -d --entitlements ":-" "$APP" > "$ENTITLEMENTS" 2>/dev/null || [ ! -s "$ENTITLEMENTS" ]; then
        echo "Could not read entitlements from the existing signature on $APP; was it built with --device?" >&2
        exit 1
    fi
    SIGN_IDENTITY=$(codesign -d -vv "$APP" 2>&1 | awk -F'=' '/^Authority=/ { print $2; exit }')
    if [ -z "$SIGN_IDENTITY" ]; then
        echo "Could not determine the code signing identity already on $APP." >&2
        exit 1
    fi
    codesign --force --deep --sign "$SIGN_IDENTITY" --entitlements "$ENTITLEMENTS" "$APP"

    xcrun devicectl device install app --device "$DEVICE_ID" "$APP"
    xcrun devicectl device process launch --device "$DEVICE_ID" "$BUNDLE_ID"

    echo "Installed and launched $APP_NAME ($BUNDLE_ID) on device $DEVICE_ID"
    exit 0
fi

# Unix Makefile builds do not expand Xcode's plist substitution tokens.
# Replace the token before packaging so sideloaders see a valid identifier.
if [ -f "$APP/Info.plist" ]; then
    sed -i.bak "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$BUNDLE_ID/g" "$APP/Info.plist"
    sed -i.bak "s/\$(EXECUTABLE_NAME)/$APP_EXECUTABLE/g" "$APP/Info.plist"
    rm -f "$APP/Info.plist.bak"

    # These are intentionally changed before every publication. Without this,
    # SideStore can keep launching its old installed executable even after it
    # has downloaded a new IPA from source.json.
    plutil -replace CFBundleShortVersionString -string "$IOS_BUNDLE_VERSION" "$APP/Info.plist"
    plutil -replace CFBundleVersion -string "$IOS_BUILD_NUMBER" "$APP/Info.plist"
fi

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/voxagine-ipa.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT INT TERM
mkdir "$STAGE/Payload"
PAYLOAD_APP="$STAGE/Payload/$APP_EXECUTABLE.app"
ditto "$APP" "$PAYLOAD_APP" 2>/dev/null || cp -R "$APP" "$PAYLOAD_APP"

IPA_OUT="$STAGE/$IPA_NAME"
(cd "$STAGE" && zip -qry "$IPA_OUT" Payload)
unzip -t "$IPA_OUT" >/dev/null
case "$(unzip -l "$IPA_OUT")" in
    *"Payload/$APP_EXECUTABLE.app/$APP_EXECUTABLE"*) ;;
    *) echo "IPA is missing Payload/$APP_EXECUTABLE.app/$APP_EXECUTABLE" >&2; exit 1 ;;
esac

# Each invocation needs its own remote staging path. Concurrent/retried uploads
# otherwise write the same .new file; whichever finishes first renames it out
# from underneath the others, leaving a partial or mismatched publication.
REMOTE_TMP="$NUC_DIR/.${IPA_NAME}.new.$$"
REMOTE_UPDATER="$NUC_DIR/.update-source.$$.py"
scp_to_nuc "$IPA_OUT" "$NUC:$REMOTE_TMP"
scp_to_nuc "$ROOT/Platforms/iOS/update-source.py" "$NUC:$REMOTE_UPDATER"

# The source rewrite is a Python program rather than a sed, because it has to
# find *this app's* entry by bundle identifier - see update-source.py. Shipped
# as a file rather than inlined here: quoting a JSON-manipulating script
# through an ssh command string is how it ends up subtly wrong.
ssh_nuc "$NUC" "set -eu
  # Validate the transferred bytes before the atomic replacement. Matching
  # file sizes do not prove a ZIP survived an interrupted/racing upload.
  python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); bad = z.testzip(); assert bad is None, \"bad CRC: \" + str(bad)' '$REMOTE_TMP'
  install -m 0644 '$REMOTE_TMP' '$NUC_DIR/store/$IPA_NAME'
  ln -sfn '$NUC_DIR/store/$IPA_NAME' '$NUC_DIR/$IPA_NAME'
  size=\$(stat -c %s '$NUC_DIR/store/$IPA_NAME')
  python3 '$REMOTE_UPDATER' '$SOURCE_PATH' '$BUNDLE_ID' '$SOURCE_VERSION' '$(date +%F)' \"\$size\"
  rm -f '$REMOTE_TMP' '$REMOTE_UPDATER'
"

echo "Published $APP_NAME as $IPA_NAME (source/iOS $SOURCE_VERSION; build $IOS_BUILD_NUMBER) to $NUC:$NUC_DIR/store/$IPA_NAME"
echo "SideStore source: https://altstore.session-zero.dev/source.json"
