# Voxagine + Bit Buster

[![Build](https://github.com/joey-j7/Voxagine/actions/workflows/build.yml/badge.svg)](https://github.com/joey-j7/Voxagine/actions/workflows/build.yml)

Voxagine is a custom C++ game engine built as a second-year project at
[IGAD](https://www.igad.nl/) (Breda University of Applied Sciences). It ships
with an ImGui-based level/entity editor and a couch co-op game, **Bit
Buster**, built on top of it.

It targets **Linux, Windows, macOS, iOS and Android** on a single Vulkan
renderer; see [Port status](#port-status) for what's verified on each.

## Engine features

- **ECS** — entity/component/system architecture (`Voxagine/Source/Core/ECS`)
- **Vulkan renderer** (`Voxagine/Source/Core/Platform/Rendering`) — dynamic
  rendering, bindless textures, HLSL shaders compiled to SPIR-V (DXC or glslc)
- **Cross-platform audio** — vendored miniaudio by default, or FMOD behind a
  build flag (`Voxagine/Source/Core/Platform/Audio`)
- **Custom memory allocators** — pool/free-list allocators (`Voxagine/Source/Core/Memory`)
- **RTTR-based reflection & JSON serialization** for save/load and the editor's
  property inspector
- **In-engine editor** — entity hierarchy, inspector, entity wizard, undo/redo,
  snapping tools (`Voxagine/Source/Editor`); runs on desktop and, with a
  touch-driven pointer, on iPad
- **Optick** integration for CPU profiling

## Requirements

Every platform needs a C++17 compiler, CMake 3.21+, Ninja, Vulkan 1.3 headers
and a loader, SDL3, and DXC or glslc (shaderc + SPIRV-Tools) to compile HLSL
to SPIR-V. One or two gamepads to play Bit Buster, though keyboard works.

**Linux:**

```bash
sudo pacman -S cmake ninja vulkan-devel sdl3 directx-shader-compiler
# add vulkan-validation-layers for validation output
```

**Windows:** the Vulkan SDK supplies the loader, headers, validation layers
and DXC; SDL3 comes from vcpkg or a binary release. MSVC 2022.

**macOS:** Xcode's command line tools, plus Homebrew:

```bash
xcode-select --install
brew install cmake ninja vulkan-headers vulkan-loader molten-vk sdl3 shaderc spirv-tools
```

iOS's own Vulkan dependency (MoltenVK) is vendored in the repo — see
[Deploying to iOS](#deploying-to-ios-xcode). Android's toolchain requirements
are in [Deploying to Android](#deploying-to-android).

## Building & running

Builds go through `CMakePresets.json`, which any recent CMake, CLion or Rider
reads directly. Five desktop presets — `game`, `game-release`, `editor`,
`editor-release`, `bringup` — each writing to
`Build/<Linux|Windows|Darwin>/<Type>/<Debug|Release>/`.

```bash
cmake --preset game && cmake --build --preset game
cd Game && ../Build/<Linux|Windows|Darwin>/Game/Debug/bin/BitBuster    # run from Game/, asset paths are relative
```

Swap `game` for `editor` to build the editor, and add `-release` for an
optimised build without validation layers.

```bash
cmake --preset editor && cmake --build --preset editor
open Build/Darwin/Editor/Debug/bin/Voxagine.app                    # macOS: a real .app
cd Game && ../Build/<Linux|Windows>/Editor/Debug/bin/Voxagine       # elsewhere: run from Game/
```

On macOS the editor bundle copies its assets to
`~/Library/Application Support/Voxagine/VoxagineEditor/assets` on first
launch — that's also where anything you save ends up. To point it at the
repository's own `Game/` tree instead:

```bash
VOXAGINE_ASSET_ROOT=$PWD/Game Build/Darwin/Editor/Debug/bin/Voxagine.app/Contents/MacOS/Voxagine
```

`voxagine_bringup` is a minimal SDL3 + Vulkan target with no engine and no
assets — the quickest way to check a toolchain is set up:

```bash
cmake --preset bringup && cmake --build --preset bringup
./Build/<Linux|Windows|Darwin>/Bringup/Debug/bin/voxagine_bringup --frames 120
```

## Deploying to iOS (Xcode)

Needs a Mac with a recent stable Xcode, and either a physical iPad/iPhone or
the Simulator.

```bash
cmake --preset ios && cmake --build --preset ios                # BitBuster.app, unsigned
cmake --preset ios-editor && cmake --build --preset ios-editor  # Voxagine editor, as its own iPad app
```

These use CMake's Xcode generator, producing a real `.xcodeproj` at
`Build/iOS/<Game|Editor>/<Debug|Release>/Voxagine.xcodeproj` you can open
directly. An unsigned build compiles and links but won't install on a device.

**To install on an attached device**, set your Apple Developer team ID and
build with automatic provisioning:

```bash
cmake --preset ios -DVOXAGINE_IOS_DEVELOPMENT_TEAM=<your 10-character team ID>
xcodebuild -project Build/iOS/Game/Debug/Voxagine.xcodeproj \
           -target BitBuster -configuration Debug -allowProvisioningUpdates

xcrun devicectl device install app --device <device UDID> \
    Build/iOS/Game/Debug/bin/Debug/BitBuster.app
xcrun devicectl device process launch --device <device UDID> com.voxagine.bitbuster
```

Xcode needs an Apple ID signed in (Xcode → Settings → Accounts) to create the
provisioning profile. `xcrun devicectl list devices` lists paired devices and
their UDIDs.

For the editor, use target `VoxagineEditor`, the paths under
`Build/iOS/Editor/...`, and bundle identifier `com.voxagine.bitbuster.editor`.

For ad-hoc distribution (e.g. via SideStore/AltStore), see
`Platforms/iOS/build-and-deploy.sh`.

### The editor on iPad

Touch drives the pointer and the camera:

| gesture | does |
|---|---|
| one finger | pointer — menus, panels, gizmo, click to select |
| two fingers, drag | orbit the camera; scrolls the panel when over one |
| two fingers, pinch | zoom |
| three fingers, drag | pan |

A Magic Keyboard, trackpad or paired mouse works as it does on desktop. File
dialogs are restricted to the project's own content tree.

## Deploying to Android

Needs JDK 17, the Android SDK (platform 34 + build-tools, `ANDROID_HOME` set)
and NDK r27c (`ANDROID_NDK_HOME`).

```bash
cd Platforms/Android
./fetch-sdl.sh                      # once
export JAVA_HOME=...                # JDK 17
export ANDROID_HOME=...
export ANDROID_NDK_HOME=...
./gradlew assembleDebug
```

The APK lands in `Platforms/Android/app/build/outputs/apk/debug/`,
`arm64-v8a` only. To check just that the engine cross-compiles, without
Gradle:

```bash
cmake --preset android-arm64-release && cmake --build --preset android-arm64-release
```

The editor is not supported on Android. See `Platforms/Android/README.md` for
the full toolchain setup and known issues.

## Port status

**Linux.** Bit Buster boots, renders the voxel world, sprites and text, plays
at ~200 fps, resizes, switches levels and exits cleanly, with zero Vulkan
validation errors. The editor builds and starts but hangs part-way through
loading assets.

**Windows.** The game and editor both build and run.

**macOS.** The game and editor both build and run — the game as a plain
executable from `Game/`, the editor as a double-clickable `Voxagine.app`.

**iOS.** The game builds, installs and runs natively on iPad (verified on an
iPad Pro, A12Z, at the display's full 2048×1536) — world, character models,
sprites and text all render correctly. The editor targets iPad as its own app
bundle with a touch-driven pointer and a UIKit file picker.

**Android.** The native library (`libmain.so`) cross-compiles and links clean
for `arm64-v8a`, checked on every CI run. The Gradle packaging has been built
and installed on a real device — see `Platforms/Android/README.md`. The
editor is not supported.

| Dependency | State |
|------------|-------|
| FMOD | Off by default (`VOXAGINE_ENABLE_FMOD`); needs the SDK downloaded by hand. miniaudio is the default and needs nothing extra. |
| Optick | Off by default (`VOXAGINE_ENABLE_OPTICK`); Windows-only. |

## Repository layout

| Path                    | Contents                                             |
|--------------------------|-------------------------------------------------------|
| `Voxagine/`              | Engine core + editor                                  |
| `Game/`                  | Bit Buster, built on Voxagine                         |
| `Platforms/`             | Per-platform packaging: Android Gradle project, iOS/macOS `Info.plist` templates, iOS deploy script |
| `SplodyMcSplodeFace/`    | An earlier game built on an earlier version of the engine |
| `UnitTesting/`           | Unit tests (allocators, reflection, physics, pathfinding, lighting) |
| `CMake/`                 | Build helper scripts                                  |

## Status

Completed as coursework and no longer under active development, apart from
the cross-platform port. Kept here as a portfolio piece.

Contributions should stay portable: platform-specific code belongs behind the
matching `VOXAGINE_ANDROID`/`VOXAGINE_IOS`/`WIN32`/`APPLE` guard (see
`CMake/Platforms.cmake`), not in the shared path.
