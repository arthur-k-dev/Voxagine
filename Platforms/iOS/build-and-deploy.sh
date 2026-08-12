#!/bin/sh
# Build Bit Buster for iOS, package a valid IPA, and publish it to the
# SideStore source hosted on the NUC. Linux cannot compile iOS binaries, but
# can run this script with --skip-build against an existing .app bundle.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/Build/iOS/Game/Make"}
APP=${APP:-"$BUILD_DIR/bin/BitBuster.app"}
IPA_NAME=${IPA_NAME:-Voxagine.ipa}
NUC=${NUC:-joey@192.168.2.5}
NUC_DIR=${NUC_DIR:-/home/joey/altserver}
SOURCE_PATH=${SOURCE_PATH:-"$NUC_DIR/store/source.json"}
SOURCE_NAME=${SOURCE_NAME:-NUC}
APP_NAME=${APP_NAME:-Bit Buster}
VERSION=${VERSION:-0.1.$(date +%Y%m%d)}
SSH_KEY=${SSH_KEY:-}
SKIP_BUILD=0

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
    echo "Usage: $0 [--skip-build]"
    echo "  macOS: configure/build BitBuster, then package and publish it"
    echo "  Linux: use --skip-build with an existing BitBuster.app"
}

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$SKIP_BUILD" -eq 0 ]; then
    case "$(uname -s)" in
        Darwin)
            cmake -S "$ROOT" -B "$BUILD_DIR" \
                -DCMAKE_SYSTEM_NAME=iOS \
                -DCMAKE_OSX_ARCHITECTURES=arm64 \
                -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
                -DVOXAGINE_BUILD_ENGINE=ON \
                -DVOXAGINE_BUILD_EDITOR=OFF \
                -DVOXAGINE_BUILD_BRINGUP=OFF \
                -DVOXAGINE_ASSET_VERSION="$IOS_BUNDLE_VERSION" \
                -DCMAKE_BUILD_TYPE=Release
            cmake --build "$BUILD_DIR" --config Release --target BitBuster
            ;;
        *)
            echo "iOS compilation requires macOS/Xcode. On Linux use --skip-build." >&2
            exit 2
            ;;
    esac
fi

if [ ! -d "$APP" ] || [ ! -f "$APP/Info.plist" ] || [ ! -x "$APP/BitBuster" ]; then
    echo "BitBuster.app is missing or incomplete: $APP" >&2
    exit 1
fi

# Shader compilation writes generated .spv files beside the source .hlsl
# files. CMake's bundle copy is POST_BUILD on BitBuster, so it does not rerun
# when only a shader changed and the executable did not need relinking. Always
# resync the complete content tree before inspecting or packaging the bundle;
# cmake -E is deliberately used here because this path also runs on Linux.
if [ -d "$ROOT/Game" ]; then
    cmake -E copy_directory "$ROOT/Game" "$APP"
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
find "$APP" -type f -name 'BitBuster.ld_*' -delete
# Unix Makefile builds do not expand Xcode's plist substitution tokens.
# Replace the token before packaging so sideloaders see a valid identifier.
if [ -f "$APP/Info.plist" ]; then
    sed -i.bak 's/\$(PRODUCT_BUNDLE_IDENTIFIER)/com.voxagine.bitbuster/g' "$APP/Info.plist"
    sed -i.bak 's/\$(EXECUTABLE_NAME)/BitBuster/g' "$APP/Info.plist"
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
ditto "$APP" "$STAGE/Payload/BitBuster.app" 2>/dev/null || cp -R "$APP" "$STAGE/Payload/BitBuster.app"

IPA_OUT="$STAGE/$IPA_NAME"
(cd "$STAGE" && zip -qry "$IPA_OUT" Payload)
unzip -t "$IPA_OUT" >/dev/null
case "$(unzip -l "$IPA_OUT")" in
    *"Payload/BitBuster.app/BitBuster"*) ;;
    *) echo "IPA is missing Payload/BitBuster.app/BitBuster" >&2; exit 1 ;;
esac

# Each invocation needs its own remote staging path. Concurrent/retried uploads
# otherwise write the same .new file; whichever finishes first renames it out
# from underneath the others, leaving a partial or mismatched publication.
REMOTE_TMP="$NUC_DIR/.${IPA_NAME}.new.$$"
scp_to_nuc "$IPA_OUT" "$NUC:$REMOTE_TMP"
ssh_nuc "$NUC" "set -eu
  # Validate the transferred bytes before the atomic replacement. Matching
  # file sizes do not prove a ZIP survived an interrupted/racing upload.
  python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); bad = z.testzip(); assert bad is None, \"bad CRC: \" + str(bad)' '$REMOTE_TMP'
  install -m 0644 '$REMOTE_TMP' '$NUC_DIR/store/$IPA_NAME'
  ln -sfn '$NUC_DIR/store/$IPA_NAME' '$NUC_DIR/$IPA_NAME'
  size=\$(stat -c %s '$NUC_DIR/store/$IPA_NAME')
  sed -i -E \"s/\\\"size\\\": [0-9]+/\\\"size\\\": \${size}/\" '$SOURCE_PATH'
  sed -i -E 's/\"version\": \"[^\"]+\"/\"version\": \"$SOURCE_VERSION\"/' '$SOURCE_PATH'
  sed -i -E 's/\"versionDate\": \"[^\"]+\"/\"versionDate\": \"$(date +%F)\"/' '$SOURCE_PATH'
  rm -f '$REMOTE_TMP'
"

echo "Published $IPA_NAME (source/iOS $SOURCE_VERSION; build $IOS_BUILD_NUMBER) to $NUC:$NUC_DIR/store/$IPA_NAME"
echo "SideStore source: https://altstore.session-zero.dev/source.json"
