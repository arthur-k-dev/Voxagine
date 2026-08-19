# Mobile port plan — the game, not the editor

Package "Bit Buster" for modern iOS and Android devices. **The editor is
explicitly out of scope** — an iPad-native editor is a separate, much larger
UX redesign (touch-first ImGui replacement, sandboxed file I/O, no desktop
dialogs) and is not part of this document. See *Rejected / out of scope*.

Companion to `CLAUDE.md` (general handover) and `Docs/RENDERING_PLAN.md` (renderer
fidelity/perf). Written to be executed **one phase per session, in order**, by
a future agent with no memory of the conversation that produced it.

---

## How to use this document

1. Read **The rules** and **Ground truth** below before touching anything.
   They are verified against the tree; re-verify a `file:line` before editing
   it (line numbers drift).
2. Find the first phase in **Progress** that is not `DONE`. Do **only that
   phase**. The order is load-bearing — each phase's *Why this order* section
   says what breaks if you reorder it.
3. Meet the phase's **Acceptance** criteria before marking it done. If you
   cannot, mark it `BLOCKED` with a note.
4. Update **Progress** and the phase's own notes. Record what you measured —
   the next session plans against it, not against this document's guesses.
5. Commit per phase, on a branch, with the phase number in the message.

### Model guidance

| Phase | Nature | Recommended |
|---|---|---|
| 0 Audio backend | mechanical (isolated interface swap) | Sonnet |
| 1 Decouple NFD from the game build | mechanical | Sonnet |
| 2 Cross-compile toolchains (Android/iOS) | design, build-system heavy | **Opus** |
| 3 Asset pipeline for packaged apps | design | **Opus** |
| 4 Vulkan/MoltenVK mobile compatibility | design | **Opus** |
| 5 Touch input | design (new engine feature) | **Opus** |
| 6 Aspect ratio, orientation, safe area | design | **Opus** |
| 7 Mobile memory budget | design | **Opus** |
| 8 Store packaging & CI | mechanical, high-friction | Sonnet, with a human for signing/store accounts |

### Progress

| Phase | Status | Session / commit | Notes |
|---|---|---|---|
| 0 — Replace FMOD with a free audio backend | DONE | `feat/mobile-port` | miniaudio 0.11.25 + stb_vorbis, default backend. Audible check by ear still open — see `Docs/MOBILE_PORT_LOG.md`. |
| 1 — Decouple NFD from the game build | DONE | `feat/mobile-port` | Whole editor is now conditional, not just NFD. Game binary: 0 NFD symbols. |
| 2 — Cross-compile toolchains | PARTIAL | `feat/mobile-port` | Android arm64 compiles and links (NDK r27c, API 31). APK never assembled (no JDK/SDK here); iOS is BLOCKED - needs macOS. |
| 3 — Asset pipeline for packaged apps | CODE COMPLETE, UNRUN | `feat/mobile-port` | Extract-then-chdir on both platforms. Compiles for Android; never executed on a device. |
| 4 — Vulkan/MoltenVK mobile compatibility | PARTIAL | `feat/mobile-port` | FIFO on mobile, a legible 1.3 capability floor, dead descriptor arrays deleted. Nothing measured on a device; thermals unknown. |
| 5 — Touch input | PARTIAL | `feat/mobile-port` | Twin virtual sticks + 4 buttons, reported as gamepad keys so gameplay is unchanged. No on-screen art - see the log. |
| 6 — Aspect ratio, orientation, safe area | PARTIAL | `feat/mobile-port` | Aspect lock released on mobile, safe-area insets applied to the touch layout, landscape locked in the manifest. Rotation untested. |
| 7 — Mobile memory budget | MEASURED ON DESKTOP | `feat/mobile-port` | 792 MiB peak on the largest level; 576 MiB of it is the double-buffered voxel window. The single-buffer lever is NOT a one-line change - see the log before attempting it. |
| 8 — Store packaging & CI | PARTIAL | `feat/mobile-port` | Android compile-only CI lane added and reproduced locally. No iOS lane (would never have passed once). No signing, no store actions. |

---

## The rules

1. **The `AudioContext`/`SoundReference` interface is the seam.** Both are
   pure-virtual (`AudioContext.h:11-65`, `SoundReference.h:6-40`). A new
   backend implements them; it never reaches into engine call sites. Follow
   the shape `FMODContext`/`FMODSoundReference` and
   `NullAudioContext`/`NullSoundReference` already establish.
2. **Assets resolve as bare relative paths against the process CWD**, not
   against the executable or bundle location — confirmed throughout
   (`Settings.h:94-95`, `RenderContext.cpp:694` etc., `PosixFileSystem`
   `OpenFile` at `Voxagine/Source/Core/System/Posix/PosixFileSystem.cpp:36`
   calls `fopen` on whatever string it's given, no prefixing).
   `Platform::m_BasePath` exists (`Platform.cpp:60`) but only two call sites
   use it — do not assume it is load-bearing anywhere else. Phase 3 depends on
   this being exactly true; re-verify if it has changed.
3. **The SPIR-V/C++ binding contract from `Docs/RENDERING_PLAN.md` still applies.**
   Any shader/descriptor change here (Phase 4) must keep
   `CMake/Shaders.cmake:16-19` and `VKShaderBindings.h:19-22` in lockstep.
4. **Windows and Linux desktop must keep building.** This plan adds platforms,
   it does not narrow the existing ones. New platform code goes behind
   `ANDROID`/`IOS`/`APPLE` CMake checks or their C++ equivalents, mirroring how
   `WIN32` is already handled — never in the shared path.
5. **`VOXAGINE_BUILD_EDITOR=OFF` must produce a game binary with zero desktop
   file-dialog code compiled in.** Today it doesn't (Phase 1 fixes this) — a
   mobile game target cannot link `nfd_portal.cpp`, which needs `<poll.h>` and
   a D-Bus portal (`nfd_portal.cpp:1-11`).
6. **Zero validation errors is the bar**, same as the Linux port. Run with
   validation on for every phase touched on desktop before trusting a device
   build.
7. **No new proprietary/binary-only dependencies.** The whole point of Phase 0
   is getting off exactly this pattern (FMOD: binary-only, unbuildable from
   source, licensing friction for a shipped product). Don't reintroduce it.
8. **Ask the user to look at the screen** (or hand you a device) rather than
   trying to fully automate visual verification — device behavior (touch
   latency, thermals, orientation) is not something a screenshot shows.

---

## Ground truth

Verified against the tree on 2026-08-08.

### Today's platform selection is almost nonexistent

No `APPLE`/`ANDROID`/`IOS` checks exist anywhere in `CMakeLists.txt` or
`CMake/*.cmake` (confirmed by exhaustive grep — zero hits). The only
platform branches at all are `CMakeLists.txt:161-163` (`comdlg32` on WIN32)
and `CMake/EngineSources.cmake:200-209` (`nfd_win32.cpp` vs `nfd_portal.cpp`,
**unconditional**, not gated by any option). `Settings::PlatformType` has only
`PT_LINUX`/`PT_WINDOWS` (`Platform.cpp:39-59`) and both branches do the same
SDL-based setup — there is no mobile enum value yet.

`VOXAGINE_*` build options today, all live (`CMakeLists.txt:16-23`):
`VOXAGINE_BUILD_BRINGUP` (ON), `VOXAGINE_BUILD_ENGINE` (OFF),
`VOXAGINE_BUILD_EDITOR` (ON), `VOXAGINE_FETCH_RTTR` (ON),
`VOXAGINE_CHECK_RTTR` (OFF), `VOXAGINE_ENABLE_FMOD` (OFF),
`VOXAGINE_ENABLE_OPTICK` (OFF), `VOXAGINE_ENABLE_NFD` (OFF, and misleadingly —
see Phase 1, it doesn't actually gate NFD compilation today).

### SDL3 acquisition assumes a desktop host and won't reach mobile toolchains

`CMakeLists.txt:56-61` — `find_package(SDL3 QUIET)`, falling back to
`pkg_check_modules` via pkg-config. CI builds SDL3 from source and
`sudo cmake --install`s it onto the *host* because Ubuntu doesn't package it
(`.github/workflows/build.yml:27-34`). None of this reaches an Android NDK or
iOS/Xcode toolchain — `find_package`/pkg-config only sees a desktop-installed
SDL3. Phase 2 replaces this with SDL3 built from source, per-target, via its
own CMake with the NDK/iOS toolchain files — SDL3 upstream supports both
natively; this repo has just never invoked that path.

### FMOD is not actually wired to anything today

`VOXAGINE_ENABLE_FMOD=ON` compiles `FMODContext.cpp`/`FMODSoundReference.cpp`
(`CMake/EngineSources.cmake:197-199`) against vendored headers
(`Voxagine/Source/External/FMOD/`), but **no `find_library`/link step for an
actual FMOD binary exists anywhere in CMake** — turning the flag on would fail
to link. Prebuilt Windows DLLs sit unreferenced in the tree
(`Voxagine/fmod64.dll`, `Voxagine/fmodL64.dll`,
`UnitTesting/UnitTests/fmod64.dll`, `fmodL64.dll`), plus dead PS-specific
`.prx`/`.a` blobs under the archived `SplodyMcSplodeFace/`. `CLAUDE.md`
already lists FMOD among the "Windows binaries with no source" vendored deps.
**Nothing today plays a real sound on any platform** except through
`NullAudioContext`, which is deliberately silent
(`NullAudioContext.cpp:81`).

### NFD (file dialog) leaks into the engine-common build, not just the editor

`FileBrowser.cpp` calls `NFD_SaveDialog`/`NFD_OpenDialog` directly with **no
`#ifdef EDITOR` guard** (`FileBrowser.cpp:10,25`), and `FileBrowser.h` is
included unconditionally from the engine PCH (`pch.h:21`). Every *call site*
happens to be in `Editor.cpp` (lines 660, 962, 1006, 1023, 1047, 1072, 1513),
so it's editor-only in practice — but the translation unit compiles and links
into every `VOXAGINE_BUILD_ENGINE=ON` build, editor or game. `nfd_portal.cpp`
`#include`s `<poll.h>`, `<unistd.h>`, shells out to `zenity`/`kdialog` over a
desktop D-Bus portal — none of that exists on iOS/Android. A mobile game
target cannot compile this file as-is.

### Vulkan surface creation is already portable; two things aren't mobile-tuned

- Surface: standard `SDL_Vulkan_CreateSurface` (`SDLWindowContext.cpp:108-120`)
  — no desktop-specific path, should carry to Android/iOS SDL3 builds as-is.
- Instance extensions come straight from
  `SDL_Vulkan_GetInstanceExtensions` (`SDLWindowContext.cpp:94-106`) — no
  hardcoded Linux/Windows extension names.
- Validation/`VK_EXT_debug_utils` is already conditional on `_DEBUG`
  (`VKRenderContext.cpp:39-42`, `VKDevice.cpp:49-51,62-64`) — release builds
  request neither. Nothing to change here.
- **Present mode prefers `VK_PRESENT_MODE_MAILBOX_KHR` whenever the surface
  supports it** (`VKSwapchain.cpp:66-74`, starts at FIFO, overwrites toward
  MAILBOX). MAILBOX trades power for latency — the wrong default for
  battery/thermal-constrained mobile. Phase 4 addresses this.
- **`VK_API_VERSION_1_3` is requested** (`VKDevice.cpp:60`), and the swapchain
  uses core-1.3 sync2 (`vkCmdPipelineBarrier2`, `vkQueueSubmit2` —
  `VKSwapchain.cpp:260,528`). Only `VK_KHR_SWAPCHAIN_EXTENSION_NAME` is an
  explicit device extension (`VKDevice.cpp:10,304-305`) — sync2 itself is core
  in 1.3, not a separate extension check, so **older Android devices without
  Vulkan 1.3 driver support will fail device creation outright**, not
  degrade gracefully. Phase 4 has to make a minimum-device-capability call.

### No touch input exists anywhere; gameplay is keyboard/gamepad only

Exhaustive grep for `SDL_EVENT_FINGER`/`SDL_TOUCH` — zero live hits anywhere
in the repo (only unused ImGui style fields reference "touch" in comments).
Gameplay code (`Game/Source/**`) has **zero** references to mouse state — the
mouse controller is unconditionally instantiated at the input-context level
(`InputContextNew.cpp:31,40`) and fed to ImGui, but nothing in `Game/Source`
reads it. Gameplay input goes through `SDLGamePad.cpp`/`SDLKeyboard.cpp` via
`InputHandler` (e.g. `Game/Source/Humanoids/Players/Player.cpp`). This is
better news than it sounds: there is no mouse-picking assumption baked into
gameplay to unwind, but there is also **no input path that works on a
touchscreen at all** — Phase 5 is a genuinely new feature, not a retrofit.

### Aspect ratio is locked and letterboxed, resize is event-driven

`Settings::m_fLockedAspectRatio` defaults to 16:9 (`Settings.h:132`),
consumed by `RenderContext::ConstrainToAspectRatio()`
(`RenderContext.cpp:872-889`) — always letterboxes/pillarboxes to that one
ratio, whatever the window/device is. Resize is handled through real SDL
events (`SDLWindowContext.cpp:122-163`,
`SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`) flowing to
`VKRenderContext::OnResize` → `VKSwapchain::Recreate` — so the *plumbing* for
a device-rotation resize already exists, it has just never been exercised
against a real rotation event, and CI never touches presentation at all
(`.github/workflows/build.yml:45-48`, headless runner). No safe-area/notch
handling exists anywhere.

### Memory: the two costs that matter most, and one that needs re-verifying

- The GPU voxel world buffer for a 6×6-chunk level is 288 MiB, doubled to
  **576 MiB of `HOST_VISIBLE`** memory for double buffering
  (`Docs/RENDERING_PLAN.md`'s own ground truth, `VKMapper.cpp:74-96,125-140`,
  `RenderContext.cpp:230-249,673-688`). This is a real number to budget
  against on a phone.
- **Discrepancy found and not yet resolved**: `CLAUDE.md` and
  `Docs/RENDERING_PLAN.md` both describe `Chunk::m_VoxelData` (128 MiB per loaded
  chunk) as never freed. But `Chunk.cpp:163-165`, inside `EncodeVoxels()`
  (called from `Chunk::UnloadAsync`, `Chunk.cpp:93-119`, itself invoked from
  `ChunkSystem.cpp:350` when a chunk slides out of the window), **does** call
  `m_VoxelData.resize(0); m_VoxelData.shrink_to_fit();`. Phase 7 must resolve
  this discrepancy with a real measurement before sizing a mobile memory
  budget on it — don't trust either document's framing over a profiler.

### CI builds nothing that resembles the game today

`.github/workflows/build.yml` is Ubuntu-only (`:11`), and its configure step
never passes `-DVOXAGINE_BUILD_ENGINE=ON` (`:39-43`) — so **BitBuster itself
is never built in CI**, only `voxagine_bringup` and the RTTR reflection check.
Phase 8's CI work is additive to a currently very thin baseline; don't assume
today's green CI says anything about the game compiling, mobile or otherwise.

---

## Phase 0 — Replace FMOD with a free, cross-platform audio backend

*Mechanical — isolated behind an existing interface. Sonnet.*

**Goal.** Working audio on every platform this project targets (Linux,
Windows, and the new mobile targets), with no proprietary binary dependency.

**Why first.** No platform has working sound today (see Ground truth) — this
isn't mobile-specific prep, it's a real gap that blocks the whole project, and
it's cheapest to fix before adding two more platforms that would each need
their own FMOD binary anyway.

**Recommendation: [miniaudio](https://miniaudio.dev/)** (public domain /
MIT-0, single-header C). It covers every target this repo cares about from one
dependency — CoreAudio (iOS), AAudio/OpenSL ES (Android), ALSA/PulseAudio
(Linux), WASAPI (Windows) — with no per-platform SDK to obtain, which is
exactly the property FMOD lacked here. It has a documented Vorbis decoding
extension, needed for the `.ogg` assets this project already uses (see
`CLAUDE.md`'s note on the `NullSoundReference::Load` bug, which was about
`.ogg` loads specifically). SoLoud is a reasonable second choice (also
zlib-licensed, more built-in 3D-audio helpers) if miniaudio's lower-level API
turns out to be a poor fit once you're in it — don't switch without a concrete
reason, both are free and portable.

**Steps.**

1. Vendor miniaudio under `Voxagine/Source/External/miniaudio/` (single header
   + the vorbis decoding extension header), matching how other single-header
   externals are handled in this tree.
2. Implement `MiniaudioContext : AudioContext` and
   `MiniaudioSoundReference : SoundReference`, mirroring the shape of
   `FMODContext`/`FMODSoundReference` — same method set, same object lifetime
   (`SoundReference::~SoundReference` already calls
   `m_pAudioContext->OnReferenceDestroyed(this)`, so the dangling-BGM-pointer
   fix from `CLAUDE.md` carries over unchanged as long as the new backend
   doesn't bypass that path).
3. Wire it into CMake the same way FMOD is wired
   (`CMake/EngineSources.cmake:194-199`, `CMakeLists.txt:174-183`): a new
   `VOXAGINE_AUDIO_BACKEND` option (values `NONE`/`MINIAUDIO`, replacing the
   boolean `VOXAGINE_ENABLE_FMOD`) that swaps in `Miniaudio*` sources instead
   of `Null*`, no library linkage step needed since miniaudio is header-only.
4. **Make `MINIAUDIO` the default** once it plays a sound correctly end to end
   — audio should work out of the box, not require an opt-in flag the way
   FMOD did.
5. Delete `FMODContext.*`, `FMODSoundReference.*`,
   `Voxagine/Source/External/FMOD/`, the vendored `fmod*.dll` binaries, and the
   `fmod_logo.png` splash asset. Confirm nothing else references them
   (`grep -ri fmod` across the tree) before deleting.
6. Keep `NullAudioContext`/`NullSoundReference` — a genuinely silent backend
   remains useful for CI and headless bring-up.

**Acceptance.** A world with a `.ogg` BGM and at least one 3D sound effect
plays audibly on Linux, with looping (`SetLoopPoints`) and BGM
pause/resume/stop all working — user confirms by ear. Build with the old
`VOXAGINE_ENABLE_FMOD` flag no longer exists; `grep -ri fmod` outside git
history returns nothing. Zero validation/compiler warnings introduced.

---

## Phase 1 — Decouple the desktop file dialog from the game build

*Mechanical. Sonnet.*

**Goal.** `VOXAGINE_BUILD_EDITOR=OFF` produces a binary with zero desktop
file-dialog code compiled in, so a mobile game target has nothing in it that
assumes `<poll.h>` or a Linux desktop portal.

**Why before the toolchain work.** Phase 2 tries to get *something* building
under the Android NDK and iOS toolchains. If NFD is still unconditionally
compiled, that attempt fails immediately and for the wrong reason — fix the
layering bug first so Phase 2's failures are about the toolchain, not about a
stray desktop dependency.

**Steps.**

1. In `CMake/EngineSources.cmake:200-209`, wrap the whole `nfd_win32.cpp` /
   `nfd_portal.cpp` selection in `if(VOXAGINE_BUILD_EDITOR)`. Don't add these
   sources at all when building the game only.
2. Move `FileBrowser.h`'s include out of the unconditional engine PCH
   (`pch.h:21`) — either behind `#ifdef EDITOR` in the PCH, or (cleaner) out of
   the PCH entirely, since it has exactly one consumer (`Editor.cpp`).
3. Confirm `Voxagine/Source/Core/FileBrowser.cpp` itself only compiles when
   `EDITOR` is defined — either gate the whole file in CMake alongside step 1,
   or wrap its body in `#ifdef EDITOR` to match the reality that every call
   site is already editor-only.
4. Build `build-game` (existing preset, `VOXAGINE_BUILD_EDITOR=OFF`) and
   confirm it links with no NFD symbols pulled in
   (`nm`/`objdump` the binary, or just check the link line has no
   `nfd_*.cpp.o`).

**Acceptance.** `build-game` and `build-editor` both still build and run on
Linux, zero validation errors. The game binary's object files contain no
`nfd_*` translation unit. `grep -rn VOXAGINE_NFD` — decide whether to delete
the now-doubly-dead option or wire it up properly; don't leave a flag that
does nothing (Ground truth already flagged it as misleading today).

---

## Phase 2 — Cross-compile toolchains: get something on-device

*Design, build-system heavy. **Opus**.*

**Goal.** An empty/bring-up-equivalent Vulkan window opens on a real Android
device and a real iOS device, built from this CMake tree.

**Why this order.** Nothing past this point is testable without a way to
actually run code on the target platforms. Get the toolchain plumbing working
against the smallest possible target (`voxagine_bringup`-equivalent, not the
full game) before touching gameplay, assets, or input.

**Steps — Android.**

1. Add an Android CMake toolchain invocation
   (`$ANDROID_NDK/build/cmake/android.toolchain.cmake`), targeting a specific
   `ANDROID_ABI` (`arm64-v8a` — this is the only ABI worth targeting for a
   Vulkan 1.3 game; skip `armeabi-v7a`/x86 unless a specific reason appears)
   and `ANDROID_PLATFORM` (pick a minimum API level in Phase 4, once the
   Vulkan 1.3 floor is decided — they're the same decision, make it once).
2. Build SDL3 from source **for the target**, via SDL3's own CMake with the
   same NDK toolchain file — not `find_package`/pkg-config, which only sees
   the desktop host (Ground truth, above). SDL3 upstream has first-class
   Android support including the Java shim/`SDLActivity` needed to host a
   native Vulkan surface inside an APK; use SDL3's own Android project
   template as the outer Gradle wrapper rather than hand-rolling one.
3. `CMake/RTTR.cmake` is untested against this toolchain (Ground truth). If
   `VOXAGINE_BUILD_ENGINE=ON` is required for the mobile game target — check
   whether it actually is, or whether the game can run RTTR-free — attempt the
   FetchContent build under the NDK toolchain and record what breaks. If RTTR
   itself doesn't cross-compile cleanly, that's a real finding to escalate,
   not something to route around silently.
4. Package as an APK via Gradle, embedding the built `.so` through SDL3's
   template structure. Get it running in an emulator first, then on a real
   device (an emulator will not tell you anything about Vulkan driver
   quirks — Phase 4 needs real hardware).

**Steps — iOS.**

1. Add an iOS CMake toolchain (a maintained community one, or hand-write a
   minimal one — this repo has no existing per-platform toolchain file to
   extend). Target a specific `IPHONEOS_DEPLOYMENT_TARGET` (decide alongside
   the MoltenVK/Vulkan capability check in Phase 4).
2. Build SDL3 from source for iOS via its own CMake/Xcode support — same
   reasoning as Android, `find_package` will not reach an iOS SDK.
3. MoltenVK is required for Vulkan-on-iOS — vendor or fetch the MoltenVK
   framework (LunarG's Vulkan SDK for macOS includes it) and link against it;
   this is the one genuinely new third-party dependency this plan adds, and it
   is source-available/Apache-2.0, not a repeat of the FMOD problem.
4. Produce a signed `.app`/`.ipa` for a physical device via Xcode. **Code
   signing needs a human with an Apple Developer account** — do not attempt to
   automate provisioning-profile creation; ask the user to handle enrollment
   and provide a signing identity.

**Acceptance.** On a real Android device (arm64) and a real iOS device: a
window opens, clears to a color via Vulkan, and closes cleanly — the same bar
`voxagine_bringup` already meets on Linux. Record which NDK/API level and
which iOS SDK/deployment target were used; later phases build on these
specific choices, not "whatever's newest."

**Risks.** This is the phase most likely to surface a genuine blocker (a
dependency that simply won't cross-compile, a MoltenVK feature gap). If RTTR
or SDL3 doesn't build cleanly, stop and report rather than hacking around it —
the fix likely belongs upstream or as a deliberate, documented patch, not a
silent workaround future sessions won't know about.

---

## Phase 3 — Asset pipeline for packaged apps

*Design. **Opus**.*

**Goal.** The game's ~100+ call sites that do `fopen("Engine/Assets/...")`
relative to CWD keep working, unmodified, when assets are packaged inside an
APK or an iOS app bundle instead of sitting next to the executable.

**Why after Phase 2.** This needs a real device to test against — "does this
path resolve" is meaningless without an actual packaged app to resolve it
inside.

**Design: resolve by relocating the process's asset root, not by rewriting
call sites.** Given asset paths are consistently bare-relative-to-CWD (Ground
truth, rule 2), the lowest-risk fix is to make CWD (or equivalently, prepend a
resolved base path once, centrally) point at the right place per platform,
rather than touching every `fopen`/`ifstream` call site in the codebase:

- **iOS**: the app bundle *is* a real filesystem directory, readable via plain
  C stdio. At startup, resolve `[[NSBundle mainBundle] resourcePath]` and
  `chdir()` to it (or to a subdirectory matching today's `Game/` layout) before
  any asset load happens. This should be close to a non-event.
- **Android**: packaged assets are **not** plain files — `assets/` inside an
  APK needs `AAssetManager`, not `fopen`, so this is the real work. Recommend
  an **extraction step**: on first launch, copy the bundled assets from the
  APK (`AAssetManager_openDir`/`AAsset_read` via JNI, driven from SDL3's
  Android glue, which already exposes this) into the app's private writable
  storage (`Context.getFilesDir()`), then `chdir()` there. Every existing bare
  relative path then works completely unmodified. This costs first-launch time
  and doubles on-device storage for asset data (packaged + extracted copy) —
  both are worth it for not touching a hundred call sites; revisit only if
  storage or first-launch time turns out to be a real problem on a real
  device.
- Skip re-extracting on every launch — version/hash-check against what's
  already extracted (a manifest file with a content hash is enough).
2. Confirm `teenypath.cpp`'s `NormalizeSeparators` (`teenypath.cpp:28-34`)
   doesn't need changes — it already targets `std::filesystem`, which works
   the same way on both new platforms.
3. `.wld` world saves and `UserSettings.vguser` writes need a **writable**
   location distinct from the (potentially read-only, on iOS bundle) asset
   root — check every write path (autosave, `UserSettings`) and point those
   specifically at platform-appropriate writable storage
   (`Context.getFilesDir()`/documents directory on iOS), not the extracted
   asset root if that's made read-only.

**Acceptance.** A full level loads and renders correctly on both a real
Android device and a real iOS device, sourcing every texture, shader `.spv`,
world file, and sound from the packaged app — no assets pushed manually via
`adb push` or similar for the final test. Autosave produces a file that
survives an app restart. First-launch extraction time on Android measured and
recorded (if it's multiple seconds, that's a UX problem worth flagging, not
silently accepting).

---

## Phase 4 — Vulkan/MoltenVK mobile compatibility

*Design. **Opus**.*

**Goal.** The renderer runs correctly and power-appropriately on real mobile
GPUs and drivers, not just "the triangle came up in the emulator."

**Steps.**

1. **Decide the minimum device floor now, explicitly, and write it down.**
   `VK_API_VERSION_1_3` plus core sync2 (`VKDevice.cpp:60`,
   `VKSwapchain.cpp:260,528`) is requested unconditionally — device creation
   will simply fail on anything without full Vulkan 1.3 support. Rather than
   adding a compatibility fallback path (real work, and this is a small indie
   game, not a driver-compatibility product), the recommended choice is to
   **require Vulkan 1.3** and set the Android `minSdkVersion`/iOS deployment
   target accordingly, documenting the decision here once made. This is a
   product decision as much as a technical one — surface it to the user rather
   than picking silently.
2. **Change the present-mode preference for mobile.** `VKSwapchain.cpp:66-74`
   unconditionally prefers `MAILBOX` over `FIFO` when available — right for a
   desktop chasing latency, wrong for a phone's battery and thermals under
   sustained play. Make this platform-aware: prefer `FIFO` on
   Android/iOS, keep the existing `MAILBOX`-preferred behavior on
   Linux/Windows. This is a small, contained change — don't over-engineer it
   into a runtime-configurable setting unless a real need shows up.
3. **Resolve the unbounded-descriptor-array question before it becomes a
   silent pipeline-creation failure.** Both live voxel shaders declare
   `VOXEL_BUFFER voxelModelData[] : register(t3){}` /`register(t2){}`
   (`VoxelRenderer.ps.hlsl:10`, `VoxelRenderer.vs.hlsl:47`) — an unbounded
   typed-buffer descriptor array, compiled into the live SPIR-V, even though
   nothing in either shader body reads from it (dead, per
   `Docs/RENDERING_PLAN.md`'s independent finding on the abandoned GPU-baker path).
   Confirm on real Android and iOS/MoltenVK hardware that pipeline creation
   with an empty/unbound descriptor of this shape actually succeeds — some
   mobile Vulkan implementations and MoltenVK's descriptor-indexing support
   are pickier than desktop drivers about this even when it's never sampled.
   If it's a problem, the fix is cheap: delete the two dead declarations
   entirely (nothing references them) rather than working around a
   descriptor-indexing limitation for code that does nothing.
4. Confirm `scalarBlockLayout` (required by `-fvk-use-dx-layout`, per
   `Docs/RENDERING_PLAN.md` rule 2) is actually supported and enabled on target
   mobile devices — it's a device feature, not guaranteed everywhere Vulkan
   1.3 is. If a target device lacks it, that's a hard blocker worth
   discovering here, not mid-Phase-5.
5. Re-run the existing bring-up validation bar: zero validation errors on
   device, both platforms.

**Acceptance.** The full game (not bring-up) renders correctly on one real
Android device and one real iOS device with `FIFO` present mode, zero
validation errors, and a recorded frame rate. Minimum OS/API level decision
written into this document's Ground truth section for future phases to build
on.

**Risk — thermals.** Even with `FIFO`, marching 288–576 MiB of host-visible
memory per frame (see `Docs/RENDERING_PLAN.md`'s cost model) is a much bigger ask
of a phone's power budget than a desktop GPU's. This phase should record
device temperature/throttling behavior after a few minutes of sustained play,
not just instantaneous frame time — `Docs/RENDERING_PLAN.md`'s optimization phases
(occupancy bricks, depth prepass) are directly relevant to mobile thermals and
should be considered a soft prerequisite if this phase finds throttling.

---

## Phase 5 — Touch input

*Design — a genuinely new engine feature, not a retrofit. **Opus**.*

**Goal.** The game is playable with touch as the only input method.

**Why this is more than "add finger events."** Ground truth confirmed no
touch handling exists at all, and gameplay currently assumes a keyboard or
gamepad is present via `InputHandler`. There is no existing on-screen UI
layer this can lean on — and per explicit direction, **this plan does not
build on ImGui for it** (ImGui isn't touch-friendly, and a full touch-native UI
system is a separate, already-planned future initiative — see *Rejected / out
of scope*). Scope this phase as a minimal, bespoke stopgap that does not try
to anticipate that future system's design.

**Steps.**

1. Add `SDL_EVENT_FINGER_DOWN`/`_UP`/`_MOTION` handling to the input layer,
   alongside the existing `SDLGamePad`/`SDLKeyboard` pattern — a new
   `SDLTouch` input source feeding `InputContextNew`
   (`InputContextNew.cpp:31,40` is the pattern to follow for wiring a new
   controller in).
2. Design a minimal on-screen control scheme for Bit Buster's actual control
   surface (read `Game/Source/Humanoids/Players/Player.cpp` and its
   `InputHandler` bindings to enumerate what's actually needed — movement,
   and whatever action buttons the game uses). Render it as plain textured
   quads through the existing UI/sprite pass, not a new UI framework.
3. Map touch regions to the same logical input events gameplay already
   consumes, so `Player.cpp` and friends need **no changes** — touch should
   look like "another controller" to gameplay code, exactly the way gamepad
   and keyboard already coexist.
4. Decide, with the user, whether virtual-stick or tap-to-move (or both) fits
   this game's actual mechanics — this is a design call, not a technical one,
   and shouldn't be made unilaterally mid-implementation.

**Acceptance.** The game is fully playable on a touchscreen device with no
keyboard or gamepad attached — user confirms by playing it. Existing
keyboard/gamepad input paths are unaffected (test on Linux desktop after this
phase, not just on-device).

---

## Phase 6 — Aspect ratio, orientation, and safe area

*Design. **Opus**.*

**Goal.** The game looks correct across the wide range of phone/tablet screen
sizes and aspect ratios, including live orientation changes, without
letterboxing everything to 16:9.

**Steps.**

1. Decide (with the user) whether Bit Buster should support both orientations
   or lock to one — this changes what "correct" means for the rest of the
   phase and is a design call.
2. Extend or replace `ConstrainToAspectRatio()`
   (`RenderContext.cpp:872-889`) so it handles the actual device aspect ratio
   sanely instead of always locking to `m_fLockedAspectRatio`'s 16:9 — likely
   widening the *playable* camera view rather than adding letterbox bars on
   an 19.5:9 phone screen, but this is a gameplay-camera decision as much as a
   rendering one; involve the user before committing to an approach.
3. Exercise the existing `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` →
   `VKSwapchain::Recreate` path (Ground truth confirms it exists but has never
   been triggered by a real rotation event) against an actual device rotation.
   Fix whatever breaks — this is genuinely untested code, treat it as such.
4. Add safe-area/notch inset handling — query it via SDL3's platform-provided
   safe-area APIs where available, and keep essential UI (once Phase 5's
   control overlay exists) clear of the inset region.

**Acceptance.** Rotating a real device mid-game doesn't crash, black-screen, or
misplace the render target; the image fills the screen sensibly at a
non-16:9 aspect ratio; on-screen controls (Phase 5) stay clear of notches/home
indicators. User confirms on an actual device.

---

## Phase 7 — Mobile memory budget

*Design. **Opus**.*

**Goal.** The game fits comfortably in a phone's RAM budget, which is a much
smaller and more variable target than a desktop's.

**Steps.**

1. **Resolve the `Chunk::m_VoxelData` discrepancy first**, with a profiler, not
   by trusting either existing document. `Chunk.cpp:163-165` appears to free
   the buffer on unload already — determine whether that path actually fires
   in practice (does every chunk that loads eventually unload before the app
   is measured?), and correct `CLAUDE.md`/`Docs/RENDERING_PLAN.md` if their "never
   freed" framing turns out to be stale.
2. Measure actual peak RSS on a real mid-tier Android device and a real iOS
   device, on the largest shipped level, and record it here.
3. If the 576 MiB double-buffered GPU voxel allocation (Ground truth) is a
   real problem on target devices, the cheapest lever is disabling
   back-buffering for the voxel mapper on mobile
   (`m_bHasBackBuffer = false` path already exists in `Mapper`/`VKMapper` —
   this trades a stutter on chunk-window shifts for half the memory) rather
   than a structural rewrite. Only reach for something bigger if this measured
   insufficient.
4. Cross-reference `Docs/RENDERING_PLAN.md` phases 2 (occupancy bricks) and 4
   (far-field LOD) — both reduce memory pressure as a side effect of their
   primary (performance) goal, and if this project does both plans, sequencing
   `Docs/RENDERING_PLAN.md` phase 4's far-field volume before this phase may remove
   the need for the full 768×128×768 resident window on mobile entirely. Don't
   duplicate that plan's work here — reference it.
5. Set and document a minimum-RAM device target (e.g., "4 GB devices and up")
   based on what's actually measured, not a guess.

**Acceptance.** Peak RSS measured and recorded on real target-tier devices for
both platforms, with no OOM kills during extended play on the lowest supported
tier. A documented minimum-RAM device recommendation.

---

## Phase 8 — Store packaging & CI

*Mechanical, but high-friction (accounts, signing, review policies). Sonnet
for the mechanical parts; a human is required for anything involving
Apple/Google developer account actions.*

**Steps.**

1. App icons, splash/launch screens, and store listing assets — ask the user
   what's wanted rather than guessing at branding.
2. Android: `AndroidManifest.xml` permissions (should be minimal — this game
   needs no network/camera/location), target/min SDK versions matching
   Phase 4's decision, signing config. **Do not attempt to create or manage
   Play Console credentials or automate a store submission** — that's the
   user's account and decision.
3. iOS: `Info.plist` entries, capabilities, provisioning profile and
   distribution certificate. **Same boundary as above** — signing identity and
   App Store Connect actions are the user's to perform; you can prepare
   everything up to the point that needs their credentials.
4. Add a **headless Android build lane** to CI, mirroring the existing
   Ubuntu-only, compile-only pattern (`.github/workflows/build.yml`) — an NDK
   build can run on a standard Linux runner with no device, checking
   compilation only, same philosophy as today's "no GPU, no display" caveat.
   An iOS build lane needs a `macos-latest` GitHub-hosted runner with Xcode
   preinstalled — add it the same compile-only way; code signing should be
   skipped in CI (unsigned build, or skip the archive/export step) rather than
   smuggling a distribution certificate into CI secrets.
5. Update `CLAUDE.md`'s CI section once both lanes exist.

**Acceptance.** A green CI run compiles the game for Android (NDK) and iOS
(Xcode, unsigned) on every push, alongside the existing Linux/Windows checks.
A build artifact the user can side-load or TestFlight exists for manual
testing. No signing secrets committed or added to CI without the user having
explicitly set them up themselves.

---

## Rejected / out of scope

- **An iPad-native editor.** Touch-first ImGui replacement, sandboxed file
  I/O (no `zenity`/`kdialog`/`nfd_win32` equivalent), and a fundamentally
  different UX than a mouse-and-keyboard desktop tool. Confirmed as its own,
  larger initiative — not attempted here.
- **Deprecating ImGui / a new touch-friendly game+editor UI system, with a
  proper text renderer.** Explicitly planned by the user as a separate future
  initiative, independent of this mobile port. Phase 5 (touch input) is
  deliberately scoped as a minimal bespoke overlay so it doesn't get entangled
  with or presume the design of that future system. When that initiative
  starts, it should get its own plan document in this repo, following the
  same format as this one and `Docs/RENDERING_PLAN.md`.
  - **Ordering worth naming, without deciding it here**: if the future UI
    rewrite lands before this plan's Phase 5, replatforming touch controls
    onto it would be strictly better than maintaining a bespoke overlay
    forever. That's a real dependency between the two initiatives — whichever
    session picks up UI-system planning should read this plan's Phase 5 first.
- **Vulkan compatibility fallback for pre-1.3 devices.** Rejected in favor of
  a hard minimum-device floor (Phase 4) — building and maintaining a
  reduced-feature rendering path for older hardware is disproportionate effort
  for a small indie title. Revisit only if store analytics later show a real
  population of otherwise-interested users being excluded.
- **Automating app store submission or developer account setup.** Signing,
  provisioning, and store listing actions stay with the user per the
  Explicit-permission-required boundary on account/credential actions.
