# Mobile port — running log

Work log for the execution of `Docs/MOBILE_PORT_PLAN.md`. One section per phase, in
the order they were done. `Docs/MOBILE_PORT_PLAN.md`'s own Progress table is the
short version; this is what was actually built, what was decided along the way,
and what a human still has to check.

**Branch:** `feat/mobile-port`, off `master` at `b9e8170`.

**Written by an agent working unattended overnight**, so every "user confirms
by ear / on a device" acceptance criterion in the plan is necessarily open. The
sections below say exactly which ones, rather than quietly counting them as
done. Nothing here has run on a phone.

---

## Phase 0 — miniaudio replaces FMOD — **done, one acceptance item open**

**What was true before.** Nothing played a sound on any platform.
`VOXAGINE_ENABLE_FMOD=ON` compiled `FMODContext.cpp` against vendored headers
and there was no FMOD library anywhere in the tree to link it against, so the
flag was unbuildable; every run went through `NullAudioContext`, deliberately
silent.

**What is there now.** miniaudio 0.11.25, vendored under
`Voxagine/Source/External/miniaudio/` (public domain / MIT-0), plus stb_vorbis
1.22 from its `extras/`. `MiniaudioContext` implements the whole `AudioContext`
interface. It is the default: `VOXAGINE_AUDIO_BACKEND` is a new CMake cache
variable with values `MINIAUDIO` (default) and `NONE`, replacing the boolean
`VOXAGINE_ENABLE_FMOD`.

**Decisions worth knowing.**

- **Music streams, effects decode.** A five-minute `.ogg` decodes to ~115 MB of
  f32 PCM and there are twelve of them; decoding music is not affordable on a
  phone and is not free on a desktop. The split is decided by the flag the
  engine already passes — `bIs3D`, which is `path.find("_BGM") == npos` — so no
  new convention was invented. Effects are decoded once and each channel is an
  `ma_sound_init_copy` of the decoded buffer, which is why one gunshot asset
  playing forty times is one copy of the samples.
- **`PlatformSoundReference` replaced `FMODSoundReference`.** It is
  backend-independent: `Load`/`Free` both go through `AudioContext`
  (`CreateSound`, and a new `DestroySound`), so there is no longer a pair of
  translation units defining the same class, one per backend
  (`FMODSoundReference.cpp` vs `NullSoundReference.cpp`). `NullSoundReference.
  cpp` is gone entirely as a result. `ResourceManager` names the new type.
- **`NullAudioContext` stays and is always compiled**, selected by the new
  `AA_NONE` at runtime rather than by a rebuild.
- **A double-free was fixed on the way past.** `AudioContext::
  OnReferenceDestroyed` called `StopBGM()`, and `FMODContext::StopBGM` called
  `m_pBGMReference->Release()` — on the object currently inside its own
  destructor, which re-enters `ReferenceManager` and frees it a second time.
  The base now clears `m_pBGMReference` *before* calling `StopBGM`, and both
  backends' `StopBGM` tolerate a null reference. Never observed, because FMOD
  never ran; it would have fired on the first world switch with music playing.
- **`VOXAGINE_AUDIO_NULL_DEVICE=1`** runs the backend on miniaudio's null
  device: sounds still load and decode, the mixer still runs, nothing reaches a
  speaker. That is what a headless capture or a CI run wants — unlike `AA_NONE`
  it exercises the audio path rather than skipping it. It is also how this was
  verified without waking anybody up.
- **The spatialisation distances are a guess and are the one number here that
  needs an ear.** `k_fMinAudibleDistance = 32`, `k_fMaxAudibleDistance = 512`
  voxels, inverse rolloff, doppler off. FMOD ran on its own defaults (min 1,
  max 10000) which on this world scale would have put every effect at ~1%
  volume a metre away — but nobody ever heard it, so there is no previous
  behaviour to match. They are two constants at the top of
  `MiniaudioContext.cpp`.

**The FMOD logo had to come out of the splash screen, and it was load-bearing.**
`Game/Content/UI_Art/Splash_Screen_Sprites/fmod_logo.png` was the *last* splash
entity, and its `SplashScreen Handler` was the only one carrying
`"Main Menu World"` — which is what actually switches to the main menu and
registers the skip binding. Deleting the asset alone would have left the game
sitting on a black screen forever. The entity was removed from
`SplashScreen.wld` and the transition moved onto the Buas logo's handler, with
the world-change time brought in from 8.0 s to 5.5 s to match when that logo
has finished fading out. Verified: the game still reaches the main menu (its
character models load).

**Verified.**

- `game-release` and `editor-release` both build with zero new warnings.
- `voxagine_tests checks`: 79 run, 0 failed. One of them is new —
  `Foundation/AudioDecodeChecks.cpp`, which decodes a real shipped `.ogg`
  through the vendored miniaudio and pulls PCM frames out of it. It exists
  because Vorbis is the one format miniaudio does **not** compile in by
  default, and losing that wiring fails silently: `MA_NO_BACKEND` is
  indistinguishable from a missing file by the time it reaches
  `SoundReference::Load`. All 68 of this game's sound assets are `.ogg`.
- Headless run of the real game (`--hidden`, null device), instrumented
  temporarily to print every load: `Choose_Groove_BGM.ogg` loaded streaming,
  `biito-basuta.ogg` and `ButtonError.ogg` loaded decoded. No load failures, no
  errors, clean exit.

**Open — needs a human.** Nobody has *heard* it. The plan's acceptance is "a
world with a `.ogg` BGM and at least one 3D sound effect plays audibly on
Linux, with looping and BGM pause/resume/stop working — user confirms by ear."
Run the game normally (no `VOXAGINE_AUDIO_NULL_DEVICE`) and listen; expect the
falloff distances above to want adjusting.

---

## Phase 1 — the editor is out of the game build — **done**

**The plan asked for NFD; the problem was one layer up.** `Editor.cpp` has no
`#ifdef EDITOR` around its body — the `CMakeLists.txt` comment claiming
otherwise is wrong — and every `Editor/*.cpp` was compiled into the engine
library unconditionally. So a `VOXAGINE_BUILD_EDITOR=OFF` build contained the
entire editor, and through `Editor::m_FileBrowser` it contained
`nfd_portal.cpp`, which wants `<poll.h>` and a desktop D-Bus portal. Gating the
two `nfd_*.cpp` files alone would have turned that into an undefined symbol
rather than a fix.

**What changed.** `VOXAGINE_EDITOR_SOURCES` in `CMake/EngineSources.cmake` — 28
translation units, appended to the target only under `VOXAGINE_BUILD_EDITOR`,
alongside `Core/FileBrowser.cpp` and the platform's `nfd_*.cpp`. The nfd
selection also grew an `ANDROID`/`IOS` arm that adds neither.

**Four `Editor/` translation units deliberately stay in every build**, and the
list is in a comment there so the next person does not have to rediscover it:
`imgui/*` (the game draws its debug text through ImGui),
`Configuration/BaseSettings.cpp` (`PlayerPrefs` derives from it) and
`Configuration/Project/ProjectSettings.cpp` — which `VoxApp.cpp` reads in its
**non**-editor branch, to find the start world. That last one is the sort of
thing that only shows up as a link error.

`Core/FileBrowser.h` is out of `pch.h`. It had exactly one consumer and the pch
put it in front of all 147 translation units; `Editor.h` already included it
directly, so nothing else had to change.

`VOXAGINE_ENABLE_NFD` is deleted. It set a `VOXAGINE_NFD` define that nothing
in the tree ever read, so it was two kinds of dead at once — it did not gate
NFD compilation, and its define had no consumer.

**Verified.**

- Game binary: `nm -C` finds **0** `NFD_`/`FileBrowser` symbols; it shrank by
  1.1 MB. Editor binary still has them, and still builds and links.
- Game runs headless to a clean exit, 60 fps, no validation errors.
- `voxagine_tests checks`: 79 run, 0 failed.

---

## Phase 2 — cross-compile toolchains — **Android compiles; APK and iOS open**

### What is actually verified

**`BitBuster` cross-compiles and links for `arm64-v8a`**, Debug and Release,
against Android NDK r27c at API 31, producing a `libmain.so` that is 12.8 MB
stripped and has **zero undefined Vulkan symbols**. SDL3 and RTTR are built
from source with the same toolchain. That is the substance of this phase and it
is repeatable:

```bash
ANDROID_NDK_HOME=$HOME/Android/android-ndk-r27c cmake --preset android-arm64-release
cmake --build --preset android-arm64-release
```

**The NDK was downloaded to `~/Android/android-ndk-r27c` (2.0 GB).** It is
outside the repo and nothing references it by absolute path — `ANDROID_NDK_HOME`
is what the preset reads. Delete it freely.

### Three real defects the cross-build found

These are the reason this was worth doing rather than writing the CMake and
hoping.

1. **`Core/Math.h` included `<emmintrin.h>`.** SSE2 intrinsics, in the header
   every single engine translation unit includes, for one function
   (`ftoi_sse1`) that has **no callers** and compiles to exactly what a C cast
   does. That one line stopped the entire engine building for arm64.
2. **Removing it exposed `abs()` on a float.** `<emmintrin.h>` had been
   dragging `<cmath>` in, so `abs(fMouseWheelDelta)` in `EditorCamera.cpp` had
   the float overload visible. Without it the name resolves to C's
   `int abs(int)`: the argument truncates, and for any scroll delta under 1 the
   divisor becomes **zero**, translating the editor camera by infinity. That
   was latent on every platform and nothing had ever caught it. `<cmath>` is
   now included explicitly in `Math.h` — with a comment saying why — and the
   call site says `std::abs`.
3. **`ImGui::TextWrapped(text.c_str())`** passed world data as a format string.
   GCC allows it; the NDK's clang makes it `-Werror=format-security`. The
   non-wrapping branch beside it already had the fix and a comment explaining
   it — the wrapping branch had simply been missed.

### The big one: Vulkan is loaded at runtime now

**Android's `libvulkan.so` exports the 1.0/1.1 core and nothing newer.** This
renderer is built on core-1.3 synchronization2 and dynamic rendering, so the
link failed with five undefined symbols: `vkCmdPipelineBarrier2`,
`vkQueueSubmit2`, `vkCmdWriteTimestamp2`, `vkCmdBeginRendering`,
`vkCmdEndRendering`. There is no NDK setting for this — post-1.1 entry points
are reached through `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`, full stop.

**volk** (MIT, vendored at `vulkan-sdk-1.3.275.0` to match the NDK's headers) is
the fix. Every entry point becomes a function pointer of the same name, so no
call site changed; the thirteen files that included `<vulkan/vulkan.h>` now
include `Vulkan/VulkanAPI.h`, which is the only file that includes it. Three
calls make it work — `volkInitialize()` before `vkCreateInstance`,
`volkLoadInstance` after it, `volkLoadDevice` after device creation.

Hand-loading only the five functions needed today was the alternative, and it
was rejected because it fails again — as a link error on one platform only —
the first time somebody uses a sixth.

**Desktop was re-verified after the switch, because this is exactly the kind of
change that works everywhere except where you did not look**: game and editor
both build; the game renders at 60 fps; a Debug build with the validation
layers on reports **zero validation errors**; `voxagine_tests checks` is 79/0.

### Decisions written into the build

- **`VOXAGINE_ANDROID_MIN_API = 31`, iOS deployment target 16.0**, in
  `CMake/Platforms.cmake` next to the reasoning. The renderer *requires* Vulkan
  1.3 with no fallback, per the plan's explicit rejection of a compatibility
  path. **API 31 is a compile floor, not a runtime one** — an Android 12 device
  with a Vulkan 1.1 driver installs this and fails at device creation. Making
  that failure legible is phase 4.
- **`arm64-v8a` only.** Play requires 64-bit anyway.
- **The editor is a configure error on mobile** rather than a wall of compile
  errors.
- **SDL3 is built from source whenever cross-compiling** (`CMake/SDL3.cmake`),
  and `find_package`/pkg-config stays the desktop path. The failure mode being
  avoided is not "not found" — it is pkg-config cheerfully handing an arm64
  link the host's x86-64 library.
- **`SDL_main.h` is now included by `Game/Source/main.cpp`.** On Android and
  iOS the platform calls SDL and SDL calls us, so the header renames `main` to
  `SDL_main` there; without it the APK starts to a black screen with no error.
- **`libSDL3.so` and `librttr_core.so` are redirected to the Gradle output
  directory.** Both set their own `LIBRARY_OUTPUT_DIRECTORY` and would be left
  in `_deps/`, which is a `dlopen` failure at app startup rather than a build
  error.

### Open

- **The APK has never been assembled.** `Platforms/Android/` holds a complete Gradle
  project written against SDL3's `android-project` template — manifest with a
  hard `android.hardware.vulkan.version 0x00403000` requirement and no
  permissions at all, an `SDLActivity` subclass, a placeholder vector launcher
  icon, and `fetch-sdl.sh` which pins the same SDL tag the native build uses so
  the Java and the `.so` cannot drift apart. **None of it has run**: this
  machine has no JDK, no Android SDK and no Gradle. `Platforms/Android/README.md` says so
  at the top and lists what will break first.
- **iOS is BLOCKED and cannot be otherwise.** It needs macOS, Xcode and a
  MoltenVK build; none exist on a Linux box. What is there: an `ios` preset, a
  `voxagine_link_vulkan()` that links MoltenVK and its frameworks from
  `VOXAGINE_MOLTENVK_DIR`, and the deployment-target decision. Nothing has been
  configured, let alone compiled.
- **Nothing has run on a device.** The plan's acceptance for this phase is a
  window that clears to a colour on real hardware. Not met, and not meetable
  from here.

---

## Phase 3 — asset pipeline — **written and compiling, never executed**

`Core/System/MobileAssets.{h,cpp}`, called as the first statement of `main()`,
before the launch options are even parsed.

**The approach the plan recommended, taken for both platforms rather than one.**
On first launch the packaged asset tree is copied into the app's private
writable directory and the process `chdir()`s there; every bare relative
`fopen` in the engine then resolves unmodified. A stamp file holding
`VOXAGINE_ASSET_VERSION` (Gradle passes the app's `versionName`) makes it
happen once per install rather than once per launch.

**Why iOS extracts too, when the plan expected a plain `chdir` to the bundle.**
The bundle is read-only, and this engine *writes* through the same relative
paths it reads: `Application.cpp` rewrites `Settings.vgs` when it fails to
parse, and PlayerPrefs saves the high-score table. One working directory has to
serve both, so either the writes get their own resolver - a second path
convention, in a codebase whose defining property is that it has none - or the
assets get copied somewhere writable. The copy is the smaller change and it
makes both platforms behave identically, which matters more than the disk it
costs.

**What it costs, plainly:** a second copy of ~100 MB of content on the device,
and one slow first launch of unknown length. Both are the price of not
rewriting a hundred call sites; both are revisitable once a device says
whether they matter.

**Two details that are easy to get wrong and are worth not rediscovering:**

- **Android's C asset API cannot enumerate directories.**
  `AAssetManager_openDir` returns files only, never subdirectories, so a
  recursive walk through it is impossible. The Java `AssetManager.list()` does
  return both, so enumeration goes through JNI and only the reads use the C
  API.
- **Entries carry no type**, so each is *tried* as a file and treated as a
  directory only if the open fails. The alternative - asking `list()` whether
  an entry has children - is a JNI round trip per entry across several thousand
  files. The ambiguous case is an empty directory, and aapt drops those.
- **`SDL_GetAndroidActivity()` returns a local reference the caller must
  release**, which its header says and which is exactly the sort of thing that
  leaks silently. Found by reading it rather than by running anything.

**Verified.** It compiles for Android arm64 (the JNI included) and desktop, and
the desktop path is a no-op that returns true - deliberately, with no silent
`chdir`, so a desktop run from the wrong directory keeps failing the way it
always has.

**Open — and this is the honest state of it: not one line of the extraction has
ever run.** No device, no emulator, no APK. The failure modes to look for first
are a partial extraction leaving a valid stamp (it writes the stamp last, so
this should not happen), aapt having compressed something `AAsset_read` then
returns short on, and the loose files beside the content roots
(`Settings.vgs`, `ProjectSettings.vgps`) not being where the code expects
inside the APK. The plan's acceptance - a level loading on a real device from
packaged assets, and an autosave surviving a restart - is not met.

---

## Phase 4 — Vulkan mobile compatibility — **the desk-side half is done**

**FIFO is now unconditional on mobile.** `EnableVSync` still chooses on
desktop; on a phone mailbox means rendering frames the panel will never show,
which is heat and battery spent on nothing - and a device that thermally
throttles ends up with *worse* latency than the one mailbox was chasing.

**The Vulkan 1.3 floor is now legible instead of fatal-and-silent.** Before
this, a device missing anything the renderer needs got as far as
`vkCreateDevice` and failed with the string "vkCreateDevice failed" - which on
a phone is the entire diagnostic anyone would ever have.
`HasRequiredCapabilities()` checks, up front and by name: Vulkan 1.3,
`synchronization2`, `dynamicRendering`, `timelineSemaphore`,
`bufferDeviceAddress`, the three descriptor-indexing bits the bindless texture
array needs, `scalarBlockLayout`, and the three core store/atomic features.
Devices that fail are skipped and the failure message lists what each one
lacked, ending with "Bit Buster needs Vulkan 1.3; there is no fallback path."

`scalarBlockLayout` was worth checking specifically, and it is the one in that
list whose absence would *not* have crashed: the shaders are compiled with
`-fvk-use-dx-layout`, so without it the packed `float3`s straddling 16-byte
boundaries read from the wrong offsets and the frame is quietly wrong rather
than absent.

**The dead unbounded descriptor arrays are gone.** All five shaders that
declared `VOXEL_BUFFER voxelModelData[] : register(tN)` declared it and never
read it - the abandoned GPU-baker path - and every one was the highest `t`
register in its file, so removing them shifts no other binding. They compiled
into the live SPIR-V as unbounded typed-buffer descriptor arrays, which is
exactly the shape mobile drivers and MoltenVK are pickiest about, bound to
nothing. Deleting dead code beats working around a descriptor-indexing
limitation for it.

**Verified on desktop:** shaders recompile and pass `spirv-val`, the game runs
at 60 fps, a Debug run with validation on reports zero errors.

**A screenshot comparison here proved nothing, and that is worth recording.**
Before and after images of `Training_Ground` differ in 8.5% of bytes - but two
runs of the *same* binary differ too, because the level is full of animated
entities with their own randomness. The declarations being provably unread,
and being the last register in each file, is the actual evidence; the images
are not.

**A segfault turned up and was self-inflicted.** One run in ten died, and the
cause was two instances of the game running at once - a background crash hunt
overlapping the foreground run - each asking for its own several hundred MiB
of host-visible VRAM. The log says `[vulkan] vkAllocateMemory failed for
33554432 bytes` and the process continues past it. **That failure is not
handled anywhere**, and it is a real defect worth its own fix: on a phone,
"the allocation failed" is not an exotic condition. With a single instance
running, twelve consecutive runs were clean.

**Open — everything that needs the device.** No frame rate, no thermal
behaviour, no validation run on real hardware, and no MoltenVK anything. The
plan's risk note about marching hundreds of MiB of host-visible memory per
frame on a phone's power budget is completely untested, and it is the single
most likely thing to send this back to `Docs/RENDERING_PLAN.md`.

---

## Phases 5 and 6 — touch input, and the shape of the screen

These are one commit because the safe-area work (phase 6) lives inside the
touch layout (phase 5); splitting them by file was not possible and splitting
them by hunk would have been dishonest about what was tested together.

### Touch is a gamepad

`Core/Platform/Input/Temp/TouchController.{h,cpp}`. The design decision that
made this small: **touch reports the same `IK_GAMEPAD*` keys a pad does.** Bit
Buster is already a twin-stick game - `VoxApp.cpp` binds MoveRight/MoveForward
to the left stick, RotateRight/RotateUp to the right, Fire/Dash/Special to face
buttons - so the useful thing for touch to *be* is that gamepad. Not one line
of `Game/Source` changed, and a binding added next year works on touch without
anybody remembering it exists.

The plan asked for this decision to be made with the user (virtual stick versus
tap-to-move). It could not be, so the conservative reading won: mirror the
control scheme the game already has rather than invent a second one.

- **Sticks float.** A finger landing in the left zone becomes the move stick
  centred where it landed; the right zone, outside the buttons, is aim. A fixed
  stick has to be found by looking, and a thumb on a phone is not looking.
- **Buttons are fixed** - Fire, Dash, Special, Pause - because a button that
  moves is not a button.
- **A finger keeps its assignment until it lifts**, so sliding off the fire
  button does not silently start steering.
- Fire is `IK_GAMEPADRIGHTPADDOWN`, which is also what the menus bind `Skip_UI`
  to, so one button both fires and confirms.
- It is compiled and initialised **on every platform**, not behind a mobile
  macro: SDL reports touch from trackpads and drawing tablets too, and a path
  that only compiles where nobody can attach a debugger is a path that rots.

`InputBindingHandlerInterface` grew a fourth controller slot. It is a
non-pure virtual returning null, so nothing that has no touch had to change.

### The screen

- **The 16:9 lock is released on mobile**, the same way the editor already
  releases it. A modern phone is nearer 20:9, so a locked frame would run with
  black bars down a fifth of a screen that has no bars anywhere else; rendering
  at the device ratio widens the view instead, which for a top-down game shows
  more arena rather than stretching it.
- **Safe-area insets are applied to the touch layout**, via
  `SDL_GetWindowSafeArea`, every frame - a rotation changes it and so does a
  keyboard appearing, and it is four floats. `GetLayout()` is the design-space
  layout and `GetEffectiveLayout()` is the one actually tested against, so a
  device with no cutout runs the same code path with identical numbers.
  Deliberately, a finger *outside* the safe area still steers: the inset exists
  so controls are not drawn under a notch, not to make the edge of the screen
  dead.
- **Landscape is locked** in the manifest (`sensorLandscape`), so both
  landscape orientations work and portrait does not. That is a design call made
  without the user: a twin-stick game with two thumb sticks and four buttons
  does not fit a portrait phone. It is one attribute to change.

### Open, and the honest gap

**Nothing draws the controls.** The input layer knows exactly where they are;
no pixels say so. The floating sticks genuinely work unlabelled - that is why
they float - but the four fixed buttons do not, and a player cannot find what
is not drawn. It was left undone rather than guessed at because:

- there is no touch-control art in this project, and inventing it blind, for a
  screen nobody can look at, is worse than leaving a clean seam;
- the existing sprite pass is entity-and-texture driven, so drawing it means
  authoring content, not writing code;
- `Docs/MOBILE_PORT_PLAN.md` already names a touch-native UI system as a separate
  future initiative, and this overlay is exactly what should live in it.

The seam is `TouchController::GetEffectiveLayout()`, plus `GetMoveStick()`,
`GetAimStick()` and `HasBeenTouched()` - one source of truth, so whatever draws
the overlay draws precisely what input is testing, rather than a second copy of
the numbers that drifts.

**Also untested:** the plan's acceptance for both phases is a person playing on
a real device and a real rotation event. Neither happened. Desktop
keyboard/gamepad input was re-verified after the change (the game runs, 60 fps,
clean exit); that is the whole of what was checked.

---

## Phase 7 — memory — **measured on desktop, and one trap defused**

### The `Chunk::m_VoxelData` discrepancy is resolved: the docs were stale

`Docs/MOBILE_PORT_PLAN.md`'s ground truth flagged `CLAUDE.md` and
`Docs/RENDERING_PLAN.md` for describing the chunk voxel data as never freed, and
asked for a profiler rather than trust. Reading settles it without one:
`EncodeVoxels` ends with `m_pEncodedVoxelData.shrink_to_fit()`
(`Chunk.cpp:205`), and `DecodeVoxels` ends with `clear()` +
`shrink_to_fit()` (`:251-252`). So the `reserve(10000000)` is a **transient
encode-time spike, not a retained cost** - an unloaded chunk keeps its actual
compressed blob, a loaded one keeps none, and `m_VoxelData` and the owner slots
are shrunk on unload too. `CLAUDE.md` has been corrected in place.

### Measured

**792 MiB peak RSS**, sampled from `/proc/<pid>/VmHWM`, on
`Valley_Path_To_Castle_Beat1` (a 6×6-chunk level, the largest shipped),
1280×720, 900 frames, Release, 60 fps throughout.

The arithmetic behind that number matters more than the number:

| | |
|---|---|
| resident window | 3×3 chunks of 256², 128 tall = 768×128×768 |
| voxels | 75.5 M × 4 bytes = **288 MiB** |
| double-buffered | **576 MiB** |
| everything else | ~216 MiB |

So **roughly three quarters of the process is one buffer, and half of that is
its back buffer.** On a phone that is not VRAM in a separate pool; it is the
device's RAM.

### The plan's "cheapest lever" is not a one-line change, and this is the
warning that stops the next session losing a day to it

`Docs/MOBILE_PORT_PLAN.md` phase 7 step 3 suggests setting
`m_bHasBackBuffer = false` on the voxel mapper to halve that 576 MiB, calling
it a stutter-for-memory trade. **Flipping that flag alone silently breaks the
brick grid.**

`Mapper::SwapBuffer()` returns immediately when there is no back buffer - so it
never fires `BufferSwapped`, and `BufferSwapped` is where
`RenderContext.cpp:1162` does the *rest* of the flip:
`m_pBrickMapper->SwapBuffer()`, `m_pPyramidStaging->SwapBuffer()`,
`m_BrickGrid.Swap()`, and the `SetBuffers`/`SetDensityBuffers` that re-point
the grid at the new pair. `ChunkSystem::RenderChunk` writes into the *back*
brick grid and expects that swap. With the flag off, it writes into a back grid
that is never promoted - and a brick counted at zero is never descended into,
so the symptom is **geometry quietly missing from the image**, which is exactly
the failure `CLAUDE.md` already warns about under "The voxel buffer has a
second, coarser copy now".

Doing it properly means collapsing the write-back-then-swap protocol to
write-front for all three structures *together*, plus a fence so the CPU is not
rewriting the window the GPU is marching. That is a real piece of work, and it
should be done when a device says it is needed - not speculatively, and not by
flipping a flag.

### A minimum-RAM recommendation, with its uncertainty stated

On the evidence available - 792 MiB of process on desktop, plus an Android
runtime, plus the OS, plus whatever the system will not let a foreground app
have - **6 GB is the tier to target and 4 GB should be assumed marginal until
something measures it.** This is an extrapolation from a desktop number, not a
device measurement, and the plan's acceptance (peak RSS on real target-tier
hardware for both platforms, no OOM kills in extended play) is **not met**.

The first thing to measure on a real device is whether the 288 MiB back buffer
is affordable at all. If it is not, the paragraph above is the work.

---

## Phase 8 — packaging and CI — **Android CI is real; the rest is scaffolding**

### CI

**An Android compile-only lane is added**, Debug and Release, on the same
philosophy as the existing Linux job: no GPU, no display, no device, proving
the thing that actually breaks. It uses the NDK preinstalled on GitHub's Ubuntu
runners, builds SDL3 and RTTR from source with the same toolchain, and finishes
with the check that matters — `libmain.so` exists and has **no undefined
Vulkan symbols**, which is precisely the failure Android's 1.1-only
`libvulkan.so` produces and which the Linux job cannot see.

That check earns its place: the three defects found the first time this was
built by hand (`<emmintrin.h>` in `Core/Math.h`, the `abs()` on a float that
header had been hiding, the format string that is `-Werror` on clang) were all
invisible to the Linux job.

**The exact command sequence in the workflow was run locally and passes**, in a
clean `Build/ci-android`, from configure through the symbol check.

**One drift fixed on the way past:** CI cloned SDL3 at a hardcoded
`release-3.2.0` while the tree asks for `release-3.2.20`. The tag is now read
out of `CMake/SDL3.cmake`, so there is one place it lives.

**No iOS lane, deliberately.** A `macos-latest` job would need a MoltenVK
install and an Xcode configuration that has never been run once, anywhere. A CI
job that has never passed is not a check, it is a red mark nobody can act on.
The gap is named here instead; add the job in the same session that first gets
an iOS build working by hand.

**No Gradle job either**, for the same reason: nothing in `Platforms/Android/` has ever
been executed.

### Packaging

- **Android**: manifest with a hard `android.hardware.vulkan.version 0x00403000`
  requirement, `minSdk 31`, `targetSdk 34`, landscape, **no permissions at
  all** - this game wants none, which is also the cheapest possible answer to a
  store review. A placeholder vector launcher icon exists so the manifest
  resolves; real store art is a decision for the user, per the plan.
- **iOS**: `Platforms/iOS/Info.plist.in` and the CMake bundle wiring - landscape only,
  `metal` and `arm64` as required capabilities, no permission strings, signing
  explicitly disabled. The content tree is copied into the bundle wholesale by
  a post-build step rather than as per-file `RESOURCE` entries, because it is
  ~100 MB across thousands of files. **None of this has been configured, let
  alone built.**
- **No signing configuration, no keystore, no provisioning profile, and no
  store credentials anywhere.** That boundary is the plan's and it is kept:
  those are the user's accounts and the user's decisions.

### The size problem, named because it will not go away

`Game/` is ~100 MB before the binary, it ships as APK assets, and phase 3
extracts it again on first launch - so the installed footprint is roughly
double. Play's base-module limit will be hit. The answer is asset packs or
install-time delivery, and it is real work that wants a device and a Play
Console to iterate against.

---

## Where this leaves the port

**Superseded — written before any of this ran on a device.** Kept for the
record of what was believed true at the time; **"Handoff for the next
session" at the very end of this file is the current, accurate summary.**
Most of what follows turned out right in shape and wrong in degree: the code
was there, and then it ran, and both real bugs and a real performance number
came out of that which nothing here anticipated.

**All nine phases have been worked. Three are done, four are code-complete but
unrun on hardware, one is measured-and-documented rather than changed, and one
is genuinely blocked.** The split matters more than a count, so:

| Phase | State | What is missing |
|---|---|---|
| 0 audio | **Done** | Nobody has heard it. Falloff distances want an ear. |
| 1 editor/NFD split | **Done** | — |
| 2 toolchains | Android **done**, iOS **blocked** | No APK assembled; iOS needs macOS. |
| 3 asset pipeline | Code complete | Never executed. No device. |
| 4 Vulkan mobile | Desk-side done | No device, no thermals, no MoltenVK. |
| 5 touch | Input done | Nothing draws the controls. |
| 6 screen | Code complete | No real rotation event has ever fired. |
| 7 memory | Measured, documented | Desktop numbers only; the one lever is specified, not taken. |
| 8 packaging/CI | Android CI **done** | No signing, no store, no iOS lane. |

### The single sentence version

**The engine cross-compiles, links and passes its whole test suite for Android
arm64, and everything the game needs to run on a phone has been written — but
not one line of it has executed on a phone, and that is the whole remaining
risk.**

### What to do first, in order

1. **Assemble the APK.** `Platforms/Android/README.md` lists what to install and what
   will break first. This is the step that turns four "code complete" rows into
   either "done" or a bug list, and it needs nothing but a JDK and an SDK.
2. **Listen to the game on desktop** (phase 0's open item) — it needs no
   device and it is the only acceptance criterion currently blocked purely on a
   human being awake.
3. **Draw the touch controls.** The layout is already a single source of truth
   at `TouchController::GetEffectiveLayout()`.
4. **Then measure on the device** — peak RSS, frame time, and temperature after
   a few minutes of sustained play. That measurement decides whether phase 7's
   back-buffer work is needed and whether `Docs/RENDERING_PLAN.md` gets pulled into
   this project.

### Things found along the way that are not mobile at all

Recorded here because they are real defects in the desktop game and would
otherwise be buried in a mobile log:

- **`abs()` on a float in `EditorCamera.cpp`** resolved to `int abs(int)`, so
  any scroll delta under 1 was a division by zero — an infinite camera
  translation. Fixed.
- **`AudioContext::OnReferenceDestroyed` → `StopBGM` → `Release()`** on an
  object inside its own destructor, a second free through `ReferenceManager`.
  Fixed. It had never fired because FMOD never ran.
- **`ImGui::TextWrapped(text.c_str())`** passed world data as a format string.
  Fixed.
- **`vkAllocateMemory` failure is not handled anywhere.** The log says
  `[vulkan] vkAllocateMemory failed for 33554432 bytes` and the process carries
  on; a segfault followed. Reproduced only by running two instances at once, so
  it is not a live bug on desktop — but "the allocation failed" is an ordinary
  condition on a phone, and this is the code path that will meet it. **Not
  fixed**, and the most valuable single thing on this list.
- **`Chunk`'s encoded reserve is freed after all**, contrary to what
  `CLAUDE.md` said. Corrected there.

---

## It runs. Android, verified end to end

Everything above this line was written without a device. This section is what
changed when it actually ran, and it is the most useful section in the file.

**Toolchain**, all without sudo, all under `~/Android/`: Temurin JDK 17,
Gradle 8.9, SDK platform 34 + build-tools + platform-tools, alongside the NDK.
`Platforms/Android/README.md` has the invocation.

### The APK

`assembleDebug` worked on the first attempt. Three variants now exist:

| Variant | Size | `libmain.so` | For |
|---|---|---|---|
| `debug` | 75.8 MB | 38 MB | logcat, validation, `wrap.sh` |
| `benchmark` | 65.8 MB | 12.5 MB | **measuring** - optimised, debug-signed so it installs |
| `release` | — | — | unsigned; needs the user's key |

### Verified on an Android 34 x86_64 emulator

- **The asset extraction works, and here is the number the plan asked for:
  `869 files, 93.6 MiB, in 1.41 s`.** Not a UX problem. The stamp correctly
  skipped it on relaunch - the second run reached Vulkan 25 ms after
  `SDL_main`, which is how that was confirmed rather than assumed.
- **Runtime writes work**: `PlayerPrefs.vgprefs` appears in the extracted tree
  after a run, which is the thing the whole extract-and-chdir design exists for.
- **Vulkan device creation succeeds**, at 2400x1080, **FIFO**, in **landscape**.
- **The frame is identical to desktop.** Captured the main menu on both and
  compared: same geometry, same UI, same lighting. The character models render
  black on *both* - they are the `Main_Char_*_Idle_Black` menu assets, not a
  bug, and checking rather than assuming is the only reason that is known.

**A prediction that was wrong, in a useful direction.** I expected the
emulator's SwiftShader to fail the Vulkan 1.3 capability check. It does not:
it supplies 1.3 with `synchronization2`, `dynamicRendering`,
`timelineSemaphore`, `bufferDeviceAddress`, the descriptor-indexing bits *and*
`scalarBlockLayout`. **So the emulator is a real functional test lane** - no
cable, no device - for everything except performance.

### Three bugs that only a real run could find

1. **Android discards stdout and stderr.** Not redirects - discards. Every
   `printf` in the engine wrote into nothing, on the one platform with no
   console, no argv and no attached debugger. The first run looked like a
   silent hang and was nothing of the kind; the app was fine and had no way to
   say so. `Core/System/MobileLog.cpp` dups both onto a pipe and pumps it into
   logcat from a thread, capturing every existing call site without touching
   one - the same trade `MobileAssets` makes.
2. **SDL overrides the manifest's orientation.** With no hint set, `SDLActivity`
   requests `SCREEN_ORIENTATION_FULL_USER`, so a `sensorLandscape` manifest
   came up portrait at 1080x2400. `main()` sets `SDL_HINT_ORIENTATIONS` now.
   The manifest is still right and still worth having - it is what the store
   reads - but it is not what decides this at runtime.
3. **The swapchain promised a rotation it had not performed.** `preTransform`
   states what the app has *already* applied, and it was `caps.currentTransform`
   - true on desktop only because that is always `IDENTITY` there. A
   portrait-native phone running landscape reports `ROTATE_90`, so the
   compositor received an upright image labelled as pre-rotated and **the
   entire game displayed on its side**, menu text reading bottom to top. It
   requests `IDENTITY` now and lets the compositor rotate. The faster answer on
   mobile is to keep `currentTransform` and rotate the projection to match -
   that is every camera matrix and the letterbox maths, and it wants a device
   to measure against. Correct first.

### On performance, and why the emulator cannot answer it

**10 fps, and the number means nothing**: the emulator rasterises Vulkan on the
CPU through SwiftShader. What is worth carrying is the *shape* of the frame,
because the profiler works on device now:

| pass | ms (SwiftShader, main menu) |
|---|---|
| Sun Shadow | 290 |
| Post Processing | 190 |
| UI Renderer | 12 |
| Voxel | 1.6 |

The voxel pass is cheap here only because the menu has almost no geometry to
march. The two that dominate are **geometry-independent, full-screen fixed
costs** - a 1024² shadow map and a 2400x1080 post pass - and those are exactly
what `ResolutionScale` cuts. On a real GPU all four collapse; the ordering is a
hint about where to look first on hardware, not a measurement of it.

### Still open

- **No run on real hardware.** A Samsung S23 (8 Gen 2, Adreno 740, Vulkan 1.3,
  8 GB) is the intended target and clears every floor this port sets. Frame
  time, memory and thermals are all unmeasured.
- **A Backbone controller changes the priority of the touch overlay.** It
  presents as a standard HID gamepad, `GamePadController` already handles it,
  and the game was designed around a pad - so the missing on-screen controls no
  longer block playing it.
- **`vkAllocateMemory` failure is still unhandled**, and on a phone that is an
  ordinary condition rather than an exotic one.

---

## Four things a play session on the S23 found

Reported by the user playing with a Backbone One. All four are open unless
stated.

### 1. Stick drift — FIXED, confirmed by the user

No gamepad dead zone existed anywhere. See the commit; the Backbone's hardware
was never at fault.

### 2. Upside-down character at exactly left/right — Android only, diagnosed

**The mechanism, and why a desktop test cleared it wrongly.**
`ComputeVoxelStampTransform` quantizes rotation by round-tripping through
`glm::eulerAngles`, which computes pitch as
`atan2(2(yz + wx), w² - x² - y² + z²)`. For a pure yaw the numerator is exactly
zero and the denominator is `cos θ` - which at exactly ±90° is *mathematically*
zero. `atan2(0, +0.0)` is 0 and `atan2(0, -0.0)` is π, so the sign of a
floating-point epsilon decides between an upright model and one rotated 180° in
pitch and roll.

x86-64 GCC and aarch64 Clang do not agree on that sign, because Clang contracts
`w*w - y*y` into an FMA and rounds differently. **A test of the round trip
compiled on the desktop passes** - it was written and it does - and the same
code is wrong on the phone. Facing exactly left or right is precisely the ±90°
case, which is why it is the only orientation affected.

The fix is not to make the epsilon come out positive. It is to stop asking
`atan2` a question with no answer: detect the degenerate case (both arguments
within an epsilon of zero) and treat the rotation as the pure yaw it is. A test
for it has to run on **both** architectures to mean anything, which the Android
CI lane can now do.

### 3. The process does not pause in the background

Music keeps playing after the app loses focus. SDL delivers
`SDL_EVENT_DID_ENTER_BACKGROUND`/`WILL_ENTER_FOREGROUND` on mobile and nothing
in the engine listens - the main loop keeps running, keeps rendering and keeps
mixing. On a phone that is a battery and thermal problem as much as an
etiquette one, and Android may kill the app for it.

The fix is in `SDLWindowContext::Poll`: on background, pause the audio context
and stop submitting frames; on foreground, resume. Note that Android destroys
the rendering surface in the background, so the swapchain has to be torn down
and rebuilt around it rather than merely idled.

### 4. BGM loop points are wrong, and the cause is a sample-rate mismatch

The `.cfg` sidecars give loop start and end **in PCM frames of the file's own
sample rate** - that is what FMOD's `FMOD_TIMEUNIT_PCM` meant, and what the
values were authored against.

`MiniaudioContext` passes them straight to
`ma_data_source_set_loop_point_in_pcm_frames`, whose frames are the *engine's*
output rate. A 44.1 kHz track playing through a 48 kHz engine therefore loops
at 44100/48000 of the intended point - about 8% early, and wrong by a different
amount for every track that is not already at the engine rate. Music has to
loop sample-exactly to be seamless, so this is audible immediately.

The fix is to scale by `engineRate / fileRate` when applying the points. The
file's rate is available from the decoder before the resampler; the value
`ma_sound_get_data_format` reports on a resource-manager sound is the output
rate and is therefore *not* the one to divide by, which is the trap here.

---

## Touch cannot drive menus, and a tap-as-confirm patch was tried and reverted

Reported: nothing responds to a tap on the main menu. Real bug, and the
mechanism is worth recording even though the fix was reverted.

**Every menu in this engine (`Canvas.cpp`) is navigated by a stick/d-pad plus
a single confirm button - on every platform, with no mouse-click path at all.**
`TouchController`'s move-stick and aim-stick zones cover the entire lower 70%
of the screen, so tapping directly on a menu button - the natural phone
gesture - was read as "grab the joystick", not "press confirm". Nothing was
broken in the sense of a crash or a wrong value; the touch scheme was simply
built for gameplay (twin sticks + four fixed buttons) and never asked "what
happens when this same scheme meets a menu".

**A fix was written, built, and installed: a finger that lifts without ever
deflecting past the dead zone counts as a confirm press.** It worked in the
sense that a tap started doing something. It was reverted on request rather
than kept - "I don't like hacks for this" - and the objection is fair on its
own terms, not just deferred to: the fix could make *something* respond to a
tap, but it could not make the *right* thing respond, because there is no
hit-testing anywhere in this engine from a screen point to a specific
`UIButton`'s bounds - only one currently-focused item that Up/Down/Left/Right
moves between. So the honest behaviour of that patch was "drag to navigate,
tap anywhere to confirm whatever is currently highlighted" - a real
workaround, but the kind that reads as broken to anyone who does not already
know the rule, which is exactly what "hack" means here.

**Left as: touch does not drive menus. A gamepad does, today, correctly** -
confirmed on this same session, on a Backbone One. The real fix is proper
per-widget hit-testing, and `MOBILE_PORT_PLAN.md` already scopes that
correctly: it belongs to the touch-native UI system named in that plan's
*Rejected / out of scope* section, not to a patch bolted onto a controller
scheme that was never meant to carry it. Building that hit-test path was not
attempted here - it means giving `UIComponent`/`UIButton` a queryable
screen-space rect and a system to walk them, which is real, separate work.

---

## Handoff for the next session

Written at the end of the first session that put this on real hardware. Read
this section first; it points back into the rest of the file for detail.

### What is actually true right now

- **The game runs on Android**, verified on a Galaxy S23 (Adreno 740,
  Android 15) over both USB and wireless adb, launched from an installed APK
  (not sideloaded assets) built by `Platforms/Android`'s Gradle project. It has
  also run on an x86_64 Android 34 emulator. Both are real, repeated runs, not
  a single lucky boot.
- **It is playable end to end with a gamepad.** A Backbone One was used for a
  full session, including reaching the main menu and getting into an arena.
  Movement, aiming, and firing all work.
- **Touch does not drive menus, by design, not by omission.** Every menu in
  this engine is stick/d-pad + confirm-button navigated, on every platform,
  with no mouse-click or tap-to-select path anywhere. A tap-to-confirm patch
  was built, verified working, and reverted at the user's request because it
  could make *something* respond to a tap but not the *specific* thing tapped
  - see "touch cannot drive menus" above. Real per-widget hit-testing is
  unbuilt and belongs to the touch-native UI initiative
  `MOBILE_PORT_PLAN.md` already scopes separately.
- **Real performance numbers exist for the first time**, from the S23 in an
  arena: Voxel pass 108.8 ms → 48.0 ms (0.5 `ResolutionScale` + halved cone
  step counts on mobile), 7-8 fps → 13 fps. Sun Shadow is flat at ~21-22 ms
  regardless, now the second-largest cost in the frame and untouched - see
  "can we improve the voxel pass performance" above for the full breakdown and
  why Sun Shadow is the prime suspect for the next pass (a 1024² PCSS map
  costing nearly as much as the whole reduced Voxel pass is disproportionate;
  the filter's tap count likely scales with penumbra size rather than being
  capped the way the brick-step budget is).
- **Six device-only bugs were found and fixed**, none of which any amount of
  desktop testing could have caught: stdout discarded on Android (`MobileLog`),
  SDL overriding the manifest's orientation, the swapchain claiming a rotation
  it never performed (upside-down frame), the swapchain rebuilding forever on
  an expected `SUBOPTIMAL` result (SwiftShader never surfaces this at all -
  only real hardware did), a missing gamepad dead zone, and the pad-to-player
  mapping handing the only controller present to player two.
- **Two more were found, diagnosed, and fixed without a device confirming
  them yet**: an architecture-dependent Euler-angle degeneracy that flipped
  characters upside down at exactly ±90° yaw on aarch64/Clang only (verified
  by an isolated test against the real vendored GLM, not yet seen fixed on
  screen), and BGM loop points computed in the wrong sample rate (verified by
  reading the miniaudio API contract, not yet confirmed by ear).
- **Background pause is implemented and has never been exercised.** Nobody
  backgrounded the app during this session to watch it happen. This is the
  single largest gap between "written" and "verified" left in the whole port -
  see below.

### Do these next, in order

1. **Background the app and watch it.** Push the home button or swipe away
   mid-game, wait a few seconds, come back. Two things to confirm: the music
   actually stops (this is what the user originally reported as *not*
   happening), and the game resumes cleanly rather than crashing or showing a
   black screen - `VKRenderContext::SuspendForBackground`/
   `ResumeFromBackground` tear down and rebuild the entire Vulkan surface
   around this, and it has not been watched happen once. If it crashes,
   `adb logcat -s voxagine AndroidRuntime:E DEBUG:V` is where the answer is;
   `run-on-device.sh` already filters to it.
2. **Confirm the upside-down fix and the loop-point fix.** Face exactly left
   and exactly right in the current build; the character should stay upright
   on both (it did not before this session, only on Android). Listen to the
   BGM loop; it should no longer jump early.
3. **Attack Sun Shadow.** Read `SunShadowLookup.hlsl` before touching
   anything - the PCSS filter's tap count is the first thing to check, and
   `Docs/RENDERING_PLAN.md`'s own note (`c2f8a25`, "floor the PCSS filter
   radius instead of dropping to a single tap") is exactly the kind of prior
   art this needs to be read against before changing it again.
4. **Fix the `Particles` pipeline on Adreno**, `VkResult -13`
   (`VK_ERROR_UNKNOWN`), the vertex shader specifically (the driver's own
   error dump names `gl_VertexIndex`). No particles have ever rendered on this
   device; only baked debris.
5. **A device profiler would answer more than another hour of log-reading.**
   Snapdragon Profiler or the Adreno GPU Profiler, attached to the
   `benchmark` variant, would say directly whether the Voxel pass's remaining
   cost is bandwidth, ALU, or occupancy - none of which this session could
   measure from the outside.

### Toolchain, for continuing without re-discovering any of this

Everything under `~/Android/` and installed without sudo - Temurin JDK 17,
Gradle 8.9 (though the wrapper is committed, so this is a one-time cost),
Android SDK platform 34 + build-tools + platform-tools, NDK r27c, and an
`android-34;google_apis;x86_64` emulator image. `Platforms/Android/README.md`
has the exact environment variables and invocation.

**Wireless adb is flaky across screen-lock/sleep cycles** - the port changes
and the connection silently drops, which cost real time this session. USB is
what actually worked reliably in the end; prefer it. If wireless is the only
option, `adb mdns services` re-discovers the current port faster than
guessing, but a screen-locked phone may simply not answer at all until woken.

**The `benchmark` Gradle variant, not `debug`, is what to measure with** -
release-optimised, `VOXAGINE_PROFILE_DEFAULT=ON` baked in so the frame
profiler runs without needing `adb shell setprop` or a debuggable app.
`debug`'s `libmain.so` is 3× the size and every number from it is a floor, not
a fact.

**The shared shader-output-path hazard is real and has already bitten this
session once.** `Game/Engine/Assets/Shaders/*.spv` is not platform-qualified,
so whichever preset was configured/built *last* is what every other preset's
asset root picks up next, silently. Always do a real `cmake --build` for the
platform you are about to run before running it; an incremental "no work to
do" from Ninja does not mean the bytes are right. `CMake/Shaders.cmake` has
the full reasoning at the top of `voxagine_add_shaders`.
