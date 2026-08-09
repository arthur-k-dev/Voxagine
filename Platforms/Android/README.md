# Android packaging

The Gradle project that wraps the native build into an APK.

**Status: the native half is verified, this half is not.** `libmain.so` builds
and links for `arm64-v8a` against the NDK — that is checked, repeatedly, and
`cmake --preset android-arm64` is all it takes. Nothing in *this* directory has
ever been run, because the machine it was written on has no JDK, no Android SDK
and no Gradle. Treat every file here as a careful first draft against SDL3's
`android-project` template, not as something known to work.

## What has to be installed

- **JDK 17** (Android Gradle Plugin 8.x needs 17).
- **Android SDK** with platform 34 and build-tools; `ANDROID_HOME` set.
- **Android NDK r27c**, at `ANDROID_NDK_HOME` or installed through the SDK
  manager. Other r2x releases are likely fine; r27c is the one that was used.
- **A checkout of SDL3** at the tag the native build pins
  (`VOXAGINE_SDL3_TAG` in `CMake/SDL3.cmake`). `./fetch-sdl.sh` does this.

## Why SDL3 is a checkout rather than a fetch

The Java side and the native side have to be the *same* SDL. `org.libsdl.app`
in the APK talks to `libSDL3.so` over JNI, and a mismatch is a crash at
startup, not a build error. So one checkout under `Android/third_party/SDL`
serves both: Gradle adds its `android-project/app/src/main/java` to the source
set, and CMake is pointed at the same directory with
`VOXAGINE_SDL3_SOURCE_DIR`. `fetch-sdl.sh` is what creates it, and it is
gitignored.

## Building

```bash
./fetch-sdl.sh
ANDROID_NDK_HOME=$HOME/Android/android-ndk-r27c ./gradlew assembleDebug
```

The APK lands in `app/build/outputs/apk/debug/`.

To check only that the engine cross-compiles — which is the part that actually
breaks — skip Gradle entirely:

```bash
ANDROID_NDK_HOME=$HOME/Android/android-ndk-r27c cmake --preset android-arm64-release
cmake --build --preset android-arm64-release
```

## Known problems, in the order they will be hit

1. **The APK is far too big for Play.** `Game/` is ~100 MB of content before
   the binary, and it is packaged as assets and then *extracted again* on first
   launch (see `Docs/MOBILE_PORT_PLAN.md` phase 3), so the installed footprint is
   roughly double. Play's limit is 200 MB for an AAB's base module; asset packs
   or `install-time` delivery is the real answer and is phase 8 work.
2. **Only `arm64-v8a` is built.** Deliberate — a Vulkan 1.3 game has no
   32-bit audience — but Play requires 64-bit anyway, so this is not a
   restriction in practice.
3. **Signing is not configured.** `assembleDebug` uses the debug keystore.
   Release signing is the user's key and their decision; see
   `Docs/MOBILE_PORT_PLAN.md` phase 8.
4. **`minSdkVersion` is 31 and that is a compile floor, not a runtime one.**
   The renderer needs a Vulkan 1.3 *driver*. An Android 12 device with a
   Vulkan 1.1 driver installs this and then fails at device creation. Phase 4
   is where that failure is made to say so out loud.

---

## Update: this has now been built

The APK assembles. Everything above about "never run" applied until the
toolchain below was installed; what is still unrun is the app *on a device*.

Installed, all without sudo and all under `~/Android/`, so all of it is
deletable:

| | |
|---|---|
| JDK 17 | `~/Android/toolchain/jdk-17.*` (Temurin tarball) |
| Gradle 8.9 | `~/Android/toolchain/gradle-8.9` (the wrapper is committed, so this is only needed once) |
| SDK cmdline-tools, platform 34, build-tools 34, platform-tools | `~/Android/Sdk` |
| NDK r27c | `~/Android/android-ndk-r27c` |

```bash
export JAVA_HOME=$(echo ~/Android/toolchain/jdk-17*)
export ANDROID_HOME=~/Android/Sdk
export ANDROID_NDK_HOME=~/Android/android-ndk-r27c
export PATH=$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH

./fetch-sdl.sh          # once
./gradlew assembleDebug
```

**The APK is 75.8 MB, 975 files, `arm64-v8a`.** Contents worth knowing:
`libmain.so` (38 MB, a Debug build), `libSDL3.so`, `librttr_core_d.so`,
`libc++_shared.so`, and exactly four asset roots - `Content`, `Engine`,
`Settings.vgs`, `ProjectSettings.vgps`.

**Those four are staged deliberately** (`stageGameAssets`). Pointing
`assets.srcDirs` straight at `Game/` is the obvious thing and it is wrong: it
shipped 1273 files including the game's entire C++ source tree, the leftover
MSVC project files, two OptickCore DLLs and a local `PlayerPrefs.vgprefs`. It
built and installed without complaint. The staged list is deliberately the same
list as `k_pAssetRoots` in `Core/System/MobileAssets.cpp`, and the failure is
asymmetric: staged-but-not-extracted is dead weight, extracted-but-not-staged
is a missing asset at runtime that reads as a code bug.

### ABIs

`arm64-v8a` by default - the only one worth shipping. The emulator is x86_64,
and an arm64 image on an x86 host runs under full CPU translation, which is
fine for a launcher and useless for a voxel marcher:

```bash
./gradlew assembleDebug                       # a real phone
./gradlew assembleDebug -PabiFilters=x86_64   # the emulator
```

Each extra ABI is a full second build of the engine, SDL3 and RTTR.
