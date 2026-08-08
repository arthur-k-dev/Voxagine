# Voxagine source lookup map

Use this map when the question is conceptual and the implementation location is unknown. For exact current-file hashes, symbols, includes, and subsystem tags, query `source-manifest.jsonl`.

## Fast routes by task

| Task or question | Start here | Follow into |
| --- | --- | --- |
| Process startup, frame order, shutdown | `Voxagine/Source/Core/Application.cpp` — `Application::Run` | `Platform.cpp`, `WorldManager.cpp`, timers, `RenderContext::Present` |
| Top-level owned services | `Voxagine/Source/Core/Application.h` | member constructors/destructors and getters |
| Platform backend selection | `Voxagine/Source/Core/Platform/Platform.cpp` | window, input, Vulkan, FMOD/null audio, ImGui contexts |
| Settings load/default write | `Application::LoadSettings`, `Core/Settings.*` | generic `JsonSerializer` templates and `Settings.vgs` |
| Variable/fixed time | `Core/Utils/ChronoGameTimer.*` | `GameTimer.h`, `Application::Run` |
| World push/pop/load | `Core/ECS/WorldManager.*` | deferred queue and `World::Initialize`/`Unload` |
| World lifecycle and system order | `Core/ECS/World.*` | each `ComponentSystem` implementation |
| Entity staging, hierarchy, tags | `Core/ECS/Entity.*` | `Component.*`, `Transform.*`, `World::PreTick` |
| Reflection registration | search `RTTR_REGISTRATION` in first-party source | `JsonSerializer`, editor property renderers, metadata headers |
| Generic JSON conversion | `Core/JsonSerializer.h` | `FromJson`, `ToJson`, file templates |
| World/prefab serialization | `Core/JsonSerializer.cpp` | `DeserializeWorld`, `ValueToEntity`, `ValueToComponent`, `ResolveWorldLinks` |
| Job scheduling and teardown | `Core/Threading/JobManager.*` | `JobThread.*`, `JobQueue.*`, `Job.h` |
| Identify job consumers | search `Enqueue`, `CreateJobQueue`, `DiscardJobQueue` | world, chunk, pathfinding, physics, filesystem |
| Engine logging | `Core/LoggingSystem/LoggingSystem.*` | editor console and worker call sites |
| Resource cache/ref counting | `Core/Resources/ReferenceManager.h`, `ReferenceObject.*` | `ResourceManager.*`, texture/model/sound references |
| VOX parser and frames | `Core/Resources/Formats/VoxModel.*` | `VoxelBaker`, `VoxRenderer`, chunk/render data |
| Render orchestration | `Core/Platform/Rendering/RenderContext.*` | passes, command engines, managers, Vulkan implementation |
| Vulkan initialization/present | `Core/Platform/Rendering/Vulkan/VKRenderContext.*` | `VKCommandEngine`, Vulkan buffer/pass implementations |
| Voxel component-to-grid bake | `Core/ECS/Systems/Rendering/VoxelBaker.*` | `RenderSystem`, `VoxelStamp.h`, `RenderContext` mapped buffers |
| Renderable registration/tick | `Core/ECS/Systems/Rendering/RenderSystem.*` | `VoxRenderer`, sprites, text, particles, UI, camera |
| CPU voxel layout/owner sidecar | `Core/Platform/Rendering/RenderContext.h`, `VoxelData.h` if present | `VoxelBrickGrid.*`, `VoxelBaker`, physics/chunk consumers |
| Far-field LOD | `Core/ECS/Systems/Chunk/FarFieldBaker.*` | `Core/Platform/Rendering/FarFieldVolume.*`, chunk system |
| Chunk streaming state machine | `Core/ECS/Systems/Chunk/ChunkSystem.*` | `Chunk.*`, `ChunkUpdateGroup.*`, viewers |
| Chunk compression codec | `Core/ECS/Systems/Chunk/Chunk.cpp` — `Encode`, `Decode` | codec verifier and persisted chunk buffers |
| Physics tick/collision/destruction | `Core/ECS/Systems/Physics/PhysicsSystem.*` | colliders, bodies, shapes, particles |
| Voxel world/grid mapping | `Core/ECS/Systems/Physics/VoxelGrid.*` | `IntegrityJob`, chunk volumes, render owner data |
| Detached-island integrity | `Core/ECS/Systems/Physics/IntegrityJob.*` | `PhysicsSystem` pause/resume/destruction callbacks |
| Navigation grid scheduling | `Core/ECS/Systems/Pathfinding/Grid/PathfindingChunkGrid.*` | `ChunkBuilderJob`, grid chunks/nodes |
| Navigation chunk neighbors | `Pathfinding/Grid/PathfindingChunk.*` | `PathfindingNode.*`, grid connect phases |
| Agent flow/crowd equations | `Pathfinding/Navigation/ContinuumCrowdsGroup.*` | `PathfinderGroup.*`, `Pathfinder.*` |
| Agent component movement | `Pathfinding/Navigation/Pathfinder.*` | physics body/collider and group/grid |
| Input map ownership/dispatch | `Core/Platform/Input/Temp/InputContextNew.*` | binding maps/actions/axes and controllers |
| Action modifier matching | `InputBindingHandlerInterface.*`, `InputBindingAction.*` | concrete keyboard/mouse/gamepad handlers |
| Entity input callbacks | `Core/ECS/Components/InputHandler.*` | `InputContextNew`, player-controller swap |
| Audio backend selection | `Core/Platform/Platform.cpp`, `Audio/AudioContext.*` | `FMOD/FMODContext.*`, `NullAudioContext.*` |
| Audio source/channel lifetime | `Core/ECS/Components/AudioSource.*` | `AudioSystem.*`, sound references, platform context |
| Playlist/BGM transitions | `AudioPlaylist.*`, `AudioSystem.*` | `FMODContext` BGM/reference handling |
| Editor startup and frame | `Voxagine/Source/Editor/Editor.*` | hierarchy, inspector, console, ImGui contexts |
| Undo/redo | `Editor/UndoRedo/CommandManager.*` | entity/component/transform command types |
| Entity hierarchy UI | `Editor/EntityHierarchy/EntityHierarchy.*` | editor selection and entity commands |
| Inspector/property rendering | `Editor/EntityInspector`, `Editor/PropertyRenderer` | RTTR metadata and serializer types |
| Allocator behavior | `Core/Memory/Allocators/*` | legacy allocator GoogleTests |
| File handles and async I/O | `Core/System/FileSystem.*`, `Core/System/Posix/PosixFileSystem.*` | `JobManager`, ORBIS implementation |
| Player preferences | `Core/PlayerPrefs/PlayerPrefs.*` | typed pair definitions and filesystem serialization |
| Date/time conversion | `Core/Utils/DateTime.*` | platform-specific UTC conversion |
| Engine source inclusion | `cmake/EngineSources.cmake` | root `CMakeLists.txt`, `GameSources.cmake`, editor manifest |
| Build modes/presets | root `CMakeLists.txt`, `CMakePresets.json` | local dependency directories and CI workflow |
| CI coverage | `.github/workflows/build.yml` | preset/target definitions and test integration |
| Existing tests | `UnitTesting/UnitTests/Source/Engine` | compare with CMake; currently not registered |

## Composition roots and high-value symbols

| Symbol | Responsibility | Key dependencies |
| --- | --- | --- |
| `Application::Run` | full process lifecycle and frame loop | filesystem, logging, serializer, settings, jobs, platform, world manager, editor |
| `Platform::Initialize` | platform service construction | SDL, Vulkan, input, ImGui, audio, timers |
| `WorldManager::SwapWorlds` | executes deferred world mutations | GPU wait occurs immediately before it in `Application` |
| `World::Initialize` | creates/defaults systems and camera, starts systems | jobs, render, physics, audio, chunks, scripts, pathfinding linkage |
| `World::Unload` | discards world queue and destroys entities/systems | correct global job lifetime is required |
| `World::PreTick` | commits staged entity changes and link resolution | serializer deferred links |
| `JobManager::Initialize` | worker count and thread creation | hardware concurrency and queue scan policy |
| `JobManager::Deinitialize` | cancellation, queue/thread cleanup | world teardown order and worker synchronization |
| `JobManager::ProcessFinishedJobs` | main-thread completion callbacks | object targets must still be alive |
| `JsonSerializer::DeserializeWorld` | reconstructs systems/entities/components | RapidJSON schema, RTTR types, ID mapping |
| `JsonSerializer::ValueToEntity` | recursive prefab/entity construction | component registration and parent hierarchy |
| `JsonSerializer::ResolveWorldLinks` | converts serialized IDs to live pointers | staged entity activation and prefab remaps |
| `VoxModel::Load` / `Read` / `Free` | asset parsing and frame ownership | filesystem, palette, optional audio, mapper IDs |
| `VoxelBaker::Bake` | model/transform to global voxel grid | solid voxel arrays, scale/rotation, owner sidecar |
| `RenderContext::Present` | render-pass submission/frame advancement | required named buffers, engines, and passes |
| `VKRenderContext::Initialize` | Vulkan backend and command engines | partial initialization cleanup |
| `ChunkSystem::UpdateChunks` | streaming/render group state | viewers, queues, chunk/group lifetime |
| `Chunk::Encode` / `Decode` | persisted voxel RLE | packed voxel/color/owner semantics and input validation |
| `PhysicsSystem::ApplySphericalDestruction` | mutates voxel grid and schedules integrity | radius validation, chunk residency, integrity job lifetime |
| `IntegrityJob::Run` | finds disconnected voxel structures | concurrent grid/chunk access |
| `ChunkGrid::Tick` | schedules navigation phases and agent updates | world queue, raw group/agent references |
| `ContinuumCrowdsGroup` flow methods | density/discomfort/velocity fields | node reset, distance math, agent state |
| `InputContextNew::ProcessInputBindings` | dispatches current binding map | action/axis symmetry and controller indexing |
| `AudioSystem::PostTick` | listener/source/BGM maintenance | main camera and component lifetime |
| `Editor::Initialize` / `UnInitialize` | editor resources and subscriptions | events, views, command history, textures |
| `CommandManager::Clear` | destroys undo/redo history | container must be cleared after deletion |

## Change-impact routes

### Changing voxel memory layout

Inspect all of:

- `RenderContext` voxel/color/owner buffer definitions and allocation.
- `VoxelBaker` write paths.
- `VoxelStamp` iteration and “last position” behavior.
- `PhysicsSystem`, `VoxelGrid`, and `IntegrityJob` reads/writes.
- `Chunk::Encode`/`Decode` and its round-trip verifier.
- `ChunkSystem` near-field data and `FarFieldBaker`/`FarFieldVolume` downsampling.
- Game call sites returned by `rg "Voxel" Game/Source`.

Preserve the four-byte voxel assertion, owner sentinel semantics, sidecar alignment, brick occupancy updates, and codec audit.

### Changing world teardown

Inspect `Application::Run`, `WorldManager::ClearWorlds`, `World::Unload`, `JobManager::{DiscardJobQueue,Deinitialize,ProcessFinishedJobs}`, editor subscriptions, resource destructors, and all `Job` captures. A safe order must stop producers, cancel and join world work, deliver or suppress completions under a defined rule, destroy worlds/editor, unload resources while backends live, then stop global services.

### Changing reflection/serialization

Search both engine and game `RTTR_REGISTRATION` blocks. Update generic variant conversion, world/prefab schemas, component construction, editor property renderers, link resolution, and malformed-input tests together. Preserve backward compatibility rules deliberately; do not infer them from numeric version comparisons alone.

### Changing controller/player count

Inspect every switch on `BindingMapType`, `BMT_PLAYERCONTROLLER*`, the player-controller vector allocation, action and axis registration/dispatch, map activation, and `InputHandler::SwapPlayerHandle`. Add tests for global, aggregate, each concrete player, map destruction, and rebinding.

### Adding a new `.cpp`

Add it to the relevant explicit `cmake/*.cmake` manifest, compile both game and editor modes where applicable, and regenerate this index. The source manifest finding a file does not mean CMake compiles it.

## Exclusion boundaries

- `Voxagine/Source/External`: vendored libraries. Review only integration calls unless deliberately maintaining a fork.
- `Game/Content` and `Game/Engine/Assets`: runtime assets, not source inventory. Use dedicated schema/fuzz/asset validation for them.
- `Build`: generated/local dependency products. Never use as source-of-truth.
- `SplodyMcSplodeFace/Compressed`: distributable output, not implementation.
- `.idea`: local IDE state.

## Lookup recipes

```powershell
# Which files declare or define a symbol?
rg '"symbols":.*IntegrityJob' Docs/AI_ENGINE_INDEX/source-manifest.jsonl

# Which reviewed issues touch a source file?
rg 'PathfindingChunkGrid.cpp' Docs/AI_ENGINE_INDEX/findings.jsonl

# All concurrency boundaries
rg '"status":"concurrency-risk"' Docs/AI_ENGINE_INDEX/findings.jsonl

# All files in a subsystem
rg '"subsystem":"serialization"' Docs/AI_ENGINE_INDEX/source-manifest.jsonl

# Current exact references before editing
rg -n 'DiscardJobQueue|Deinitialize|ProcessFinishedJobs' Voxagine/Source/Core
```
