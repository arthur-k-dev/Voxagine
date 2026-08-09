#!/usr/bin/env bash
# One SDL3 checkout for both halves of the Android build - see README.md.
#
# The tag is read from CMake/SDL3.cmake rather than repeated here, because the
# Java in this checkout and the libSDL3.so the native build produces have to be
# the same SDL, and two copies of a version number is how that stops being true.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
tag="$(sed -n 's/^set(VOXAGINE_SDL3_TAG "\([^"]*\)".*/\1/p' "$here/../../CMake/SDL3.cmake")"

if [ -z "$tag" ]; then
    echo "could not read VOXAGINE_SDL3_TAG from CMake/SDL3.cmake" >&2
    exit 1
fi

dest="$here/third_party/SDL"

if [ -d "$dest/.git" ]; then
    echo "SDL3 already at $dest; fetching $tag"
    git -C "$dest" fetch --depth 1 origin "$tag"
    git -C "$dest" checkout --detach FETCH_HEAD
else
    mkdir -p "$here/third_party"
    git clone --depth 1 --branch "$tag" https://github.com/libsdl-org/SDL.git "$dest"
fi

echo "SDL3 $tag ready at $dest"
