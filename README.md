# Voxagine + Bit Buster

[![Build](https://github.com/joey-j7/Voxagine/actions/workflows/build.yml/badge.svg)](https://github.com/joey-j7/Voxagine/actions/workflows/build.yml)

Voxagine is a custom C++ game engine built as a second-year project at
[IGAD](https://www.igad.nl/) (Breda University of Applied Sciences). It ships
with an ImGui-based level/entity editor and a couch co-op game, **Bit
Buster**, built on top of it.

It was originally a Windows/DirectX 12 engine. It now targets **Linux,
Windows, macOS, iOS and Android** on a single Vulkan renderer; see
[Port status](#port-status) for what currently builds, runs and is verified on
real hardware.

## Engine features

- **ECS** — entity/component/system architecture (`Voxagine/Source/Core/ECS`)
- **Vulkan renderer** (`Voxagine/Source/Core/Platform/Rendering`) — dynamic
  rendering, bindless textures, HLSL shaders compiled to SPIR-V with DXC where
  it's available and glslc (shaderc) otherwise — macOS and iOS always use the
  glslc path, since there is no DXC for those platforms
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
and a loader, SDL3, and a way to compile HLSL to SPIR-V (DXC, or glslc from
shaderc plus SPIRV-Tools as the fallback). One or two gamepads to play Bit
Buster properly, though keyboard works.

**Linux:**

```bash
# Arch
sudo pacman -S cmake ninja vulkan-devel sdl3 directx-shader-compiler
# add vulkan-validation-layers for validation output
```

The editor's file dialogs use zenity or kdialog through a desktop portal.

**Windows:** the Vulkan SDK supplies the loader, headers, validation layers
and DXC; SDL3 comes from vcpkg or a binary release. MSVC 2022 is the expected
compiler. The editor's file dialogs use Win32 `GetOpenFileName`.

**macOS:** Xcode's command line tools for the compiler, plus Homebrew for the
rest — there is no DXC on macOS, so the build always takes the glslc path
here:

```bash
xcode-select --install
brew install cmake ninja vulkan-headers vulkan-loader molten-vk sdl3 shaderc spirv-tools
```

`molten-vk`/`vulkan-loader` give the desktop macOS build a Vulkan loader over
Metal. iOS builds separately vendor their own copy of MoltenVK under
`External/MoltenVK`, so no extra Vulkan SDK install is needed for those — see
[Deploying to iOS](#deploying-to-ios-xcode).

iOS's and Android's own toolchain requirements are in their sections below.

## Building & running

Builds go through `CMakePresets.json`, which any recent CMake, CLion or Rider
reads directly. Five desktop presets - `game`, `game-release`, `editor`,
`editor-release`, `bringup` - each writing to
`Build/<Linux|Windows|Darwin>/<Type>/<Debug|Release>/` (CMake's
`hostSystemName` for macOS is `Darwin`).

```bash
cmake --preset game && cmake --build --preset game
cd Game && ../Build/<Linux|Windows|Darwin>/Game/Debug/bin/BitBuster    # run from Game/, asset paths are relative
```

The editor is a separate target rather than the same executable with a define
flipped: `EDITOR` is set on the engine library, so an editor build is a
different compilation of it, and on macOS and iPad the result is an application
bundle rather than a bare executable. Swap `game` for `editor`. Add `-release`
for an optimised build - that also drops the Vulkan validation layers, which
are tied to Debug.

```bash
cmake --preset editor && cmake --build --preset editor
open Build/Darwin/Editor/Debug/bin/Voxagine.app                    # macOS: a real .app
cd Game && ../Build/<Linux|Windows>/Editor/Debug/bin/Voxagine       # elsewhere: run from Game/
```

A double-clicked bundle starts with a working directory of `/`, so it cannot
find assets by the relative paths the engine uses everywhere. `Voxagine.app`
therefore ships the content tree inside itself and copies it, on first launch,
to `~/Library/Application Support/Voxagine/VoxagineEditor/assets` - which is
then also where anything you save from the editor ends up. To edit the
repository's own `Game/` tree instead, point it there:

```bash
VOXAGINE_ASSET_ROOT=$PWD/Game Build/Darwin/Editor/Debug/bin/Voxagine.app/Contents/MacOS/Voxagine
```

There is also `voxagine_bringup`, a standalone SDL3 + Vulkan target that clears
the screen without any of the engine. It needs no assets and pulls in no RTTR,
which makes it the quickest check that a toolchain is set up:

```bash
cmake --preset bringup && cmake --build --preset bringup
./Build/<Linux|Windows|Darwin>/Bringup/Debug/bin/voxagine_bringup --frames 120
```

## Deploying to iOS (Xcode)

Needs a Mac with a recent stable Xcode, and either a physical iPad/iPhone or
the Simulator. MoltenVK is vendored under `External/MoltenVK`, so a plain
configure needs no extra flags:

```bash
cmake --preset ios && cmake --build --preset ios                # BitBuster.app, unsigned
cmake --preset ios-editor && cmake --build --preset ios-editor  # Voxagine editor, as its own iPad app
```

`ios`/`ios-editor` (and their `-release` variants) use CMake's Xcode
generator, so the result is a real `.xcodeproj` at
`Build/iOS/<Game|Editor>/<Debug|Release>/Voxagine.xcodeproj` — open it directly
in Xcode if you'd rather work there. Building unsigned is enough to prove the
code compiles and links (this is what CI checks), but not to install on a
device.

**To install on an attached device**, reconfigure with your Apple Developer
team ID and build with automatic provisioning:

```bash
cmake --preset ios -DVOXAGINE_IOS_DEVELOPMENT_TEAM=<your 10-character team ID>
xcodebuild -project Build/iOS/Game/Debug/Voxagine.xcodeproj \
           -target BitBuster -configuration Debug -allowProvisioningUpdates

xcrun devicectl device install app --device <device UDID> \
    Build/iOS/Game/Debug/bin/Debug/BitBuster.app
xcrun devicectl device process launch --device <device UDID> com.voxagine.bitbuster
```

Xcode needs an Apple ID signed in first (Xcode → Settings → Accounts) to
create the provisioning profile on demand. `xcrun devicectl list devices`
lists paired devices and their UDIDs.

The editor follows the same signing story — swap the target for
`VoxagineEditor`, the project/app paths for the ones under `Build/iOS/Editor/...`,
and the bundle identifier for `com.voxagine.bitbuster.editor`.

For ad-hoc distribution without installing Xcode on every machine (e.g. via
SideStore/AltStore), see `Platforms/iOS/build-and-deploy.sh`, which packages a
built `.app` into a signed IPA.

### The editor on iPad

Touch drives the pointer and the camera, with finger count selecting the
gesture the way Blender uses modifiers:

| gesture | does |
|---|---|
| one finger | pointer - menus, panels, gizmo, click to select |
| two fingers, drag | orbit the camera; scrolls the panel when over one |
| two fingers, pinch | zoom |
| three fingers, drag | pan |

A Magic Keyboard, trackpad or paired mouse works as it does on desktop and is
unaffected by any of that - iPadOS delivers them as ordinary mouse and key
events. The on-screen keyboard is raised only while a text field has focus.

## Deploying to Android

Needs JDK 17, the Android SDK (platform 34 + build-tools, `ANDROID_HOME` set)
and NDK r27c (`ANDROID_NDK_HOME`). SDL3's Java and native halves have to be the
*same* checkout, which `fetch-sdl.sh` sets up:

```bash
cd Platforms/Android
./fetch-sdl.sh                      # once, pulls the SDL3 checkout both sides share
export JAVA_HOME=...                # JDK 17
export ANDROID_HOME=...
export ANDROID_NDK_HOME=...
./gradlew assembleDebug
```

The APK lands in `Platforms/Android/app/build/outputs/apk/debug/`, `arm64-v8a`
only by default. To check just that the engine cross-compiles, without Gradle:

```bash
cmake --preset android-arm64-release && cmake --build --preset android-arm64-release
```

The editor is not supported on Android — there is no native-file-dialog
backend for it there, so `Core/FileBrowser.cpp` wouldn't link. See
`Platforms/Android/README.md` for the full toolchain setup, known packaging
issues and verified device numbers.

## Port status

**Linux.** Bit Buster boots, renders the voxel world, sprites and text, plays
at ~200 fps, resizes, switches levels and exits cleanly, with zero Vulkan
validation errors. The editor builds and starts but hangs part-way through
loading assets.

**Windows is a supported target but remains unverified.** DirectX 12 is gone
and the renderer is Vulkan-only, which runs on every platform above; SDL3
covers the window, input and gamepads; the filesystem layer is plain C stdio;
and the editor's file dialogs have a Win32 implementation selected by CMake.
What is missing is that nobody has actually built it on Windows since the port
began, so expect small breakages rather than none. The MSBuild files are
deleted for good — CMake is the only build system.

**macOS.** Both the game and the editor build and run: the game as a plain
executable from `Game/`, exactly like Linux; the editor as a real
double-clickable `Voxagine.app` that finds its own assets inside the bundle.

**iOS.** The game builds, installs and runs natively on an iPad (verified on
an iPad Pro, A12Z, at the display's full 2048×1536 - not letterboxed iPhone
compatibility mode) - world, character models, sprites and text all render
correctly. The editor targets iPad as its own app bundle with the touch-driven
pointer and UIKit file-picker backend described above.

**Android.** The native library (`libmain.so`) cross-compiles and links clean
for `arm64-v8a`, checked on every CI run, including that it carries no
undefined Vulkan symbols. The Gradle packaging has been built and installed on
a real device - see `Platforms/Android/README.md` for the numbers. The editor
is not supported there yet (no file-dialog backend).

Off by default, and unchanged by any of the ports:

| Dependency | State |
|------------|-------|
| FMOD | Proprietary; needs the SDK downloaded by hand. Off behind `VOXAGINE_ENABLE_FMOD`, and audio runs silent without it. |
| Optick | Vendored headers reference a Windows-only `OptickCore.lib`. Off behind `VOXAGINE_ENABLE_OPTICK`. |

RTTR was a third: it was vendored as 126 headers with no sources and only a
Windows `rttr_core.lib`. That copy is gone; CMake uses an installed RTTR if
there is one and otherwise fetches upstream v0.9.6, so the default build still
needs no network. `nativefiledialog` and `teenypath` were a fourth and fifth —
both shipped as a header plus a Windows `.lib` — and are now implemented in
this repository, on `std::filesystem` and the platform dialog APIs.

## Repository layout

| Path                    | Contents                                             |
|--------------------------|-------------------------------------------------------|
| `Voxagine/`              | Engine core + editor                                  |
| `Game/`                  | Bit Buster, built on Voxagine                         |
| `Platforms/`             | Per-platform packaging: the Android Gradle project, iOS/macOS `Info.plist` templates and the iOS ad-hoc deploy script |
| `SplodyMcSplodeFace/`    | An earlier game built on an earlier version of the engine |
| `UnitTesting/`           | Unit tests (allocators, reflection, physics, pathfinding, lighting) |
| `CMake/`                 | Build helper scripts                                  |

## Status

This was completed as coursework and is no longer under active development,
apart from the cross-platform port (Linux, Windows, macOS, iOS and Android).
It's kept here as a portfolio piece.

Contributions should stay portable: the engine targets every platform above
from one source tree, so platform-specific code belongs behind the matching
`VOXAGINE_ANDROID`/`VOXAGINE_IOS`/`WIN32`/`APPLE` guard (see
`CMake/Platforms.cmake`), not in the shared path. `_WINDOWS` is not defined by
this build — it came from the deleted `.vcxproj` files.
