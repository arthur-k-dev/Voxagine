# Voxagine — project notes

Custom C++ game engine (Vulkan renderer, ECS, custom allocators, ImGui editor)
plus the game "Bit Buster". Originally second-year IGAD coursework, dormant
since. Current goal: **port to Linux**.

**Renderer work has its own plan: `Docs/RENDERING_PLAN.md`.** It restores the shadow
fading, AO and specular that were cut when levels were scaled up, then optimizes
to pay for them. It is phased and meant to be executed strictly in order, one
phase per session — if you are asked to improve the renderer, start there rather
than from this file.

**Destruction and the particle sim have their own plan: `Docs/DESTRUCTION_PLAN.md`.**
**All five core phases are done and both gated ones are closed by measurement**;
every entry in its defect ledger is closed except P16 (the single-buffered
particle mapper). It is still the file to read before touching destruction,
particles, integrity/islands or the loose-voxel registry — the phase notes say
what was built, what was deliberately *not* built and why.

**The first play session found four defects it had introduced**, all now fixed
and written up under "Four defects the first play session found" in that file.
Three of them share a root worth internalising: *a change that is right in the
abstract can be wrong because of something this tree does elsewhere* — and in
all three cases the "elsewhere" was already documented in this file and simply
not checked. Concretely:

- **Take a voxel's colour from the CPU voxel and a dynamic renderer gives you
  black**, because `VoxelBaker::Occupy` writes the CPU voxel only when the
  renderer is static. Every humanoid is dynamic, so every corpse came apart into
  black particles. See "Dynamic renderers are invisible to the physics grid".
- **Widening the integrity seed set reaches geometry the burst never touched**,
  and a level contains thousands of voxels that were never ground-connected to
  begin with — so a bullet clipping some rubble collapsed an entire
  non-destructible building. Seeds must be the neighbours of what was *actually
  removed*, and they must use the checker's 26-connectivity, not 6.
- **Island conversion has to check destructibility.** It never did; that was
  harmless only while seeding could not reach indestructible geometry.
- **An island falls as a shape**, so it spawns one particle per voxel. An
  explosion scatters, so its density is cosmetic. Making them "consistent" took
  three quarters of a falling roof away.

**Graphics quality is a runtime setting, not a build flag.** `Settings` holds
Shadows (Off/Hard/Soft), Ambient Occlusion (Off/Simple/Cone), Bounce Light,
Reflections, Anti-aliasing, Resolution and V-Sync; the shaders read all of them
out of the camera constant buffer each frame (`CameraData.hlsl`'s
`renderQuality`, wrapped in accessors at the top of `Defines.hlsl`). There is an
in-game settings screen - `SettingsCanvas`, text rows built in code, reached
from the pause menu and from the main menu's Settings page. `Settings::
ApplyPlatformRenderDefaults` is the *only* place a platform decides anything and
it decides defaults only: the order is `Settings.vgs` -> platform defaults ->
the player's PlayerPrefs. **Do not add a per-platform `#ifdef` for a rendering
feature** - it was tried and rejected, and it also re-opens the shared
`.spv`-path hazard, since no shader is compiled differently per platform any
more. `--render-quality low|high` prices the presets headless without touching
any file. `--ui-script` drives menus headlessly through the real SDL keyboard path
for UI regression checks - `LaunchOptions.h`. See `Docs/MOBILE_PORT_LOG.md`'s
last section for the device measurements and what is still owed.

**Chunk streaming has its own plan: `Docs/CHUNK_STREAMING_PLAN.md`; phases 0-5,
8, 9, 11 and 12 are done and phase 6 is closed as *not taken*.** A window transition's
worst frame is **150.2 -> 10.3 ms** across them - the hitch gate passes and its
`WILL_FAIL` is off - and `[world-switch] initialize` is **876 -> 317 ms**.
**Two things remain, in the order to fix them**: 13, a lifetime handle (id
+ generation, resolved on use) for the raw pointers game code holds into
streamed content, which is four of the phase 9 play session's ten defects and
ledger M8; then 10, the editor session, which needs Joey at the machine, deletes
`progressive-chunk-experiment`, and owns both of the things phase 9 left open -
**pop-in has not been judged on screen**, and `gpu_destruction_sync_stress` is
broken *on master* (its `--frames` budget expires before R1 releases gameplay,
so it has measured nothing since phase 4). The table in that plan is in
execution order, not numeric order.

**A window commit used to throw away every voxel written while it was
building**, and that is the whole of the CPU/GPU disagreement Joey saw in play
(phase 12). The render job builds the *entire* incoming window into the back
buffer from the chunks' CPU voxels and `CommitWindow` swaps it in, so a write
made in between lands in the buffer the swap retires: a debris bake is then
**invisible but solid** and a destruction clear is **visible and not there** -
a destroyed pillar coming back while collision stays correct. Destruction is
the only thing that writes voxels during play, which is why this needed phase
11's scripted combat before it could be seen headlessly at all.
`VoxelBrickGrid::SetVoxel` journals the ids while a build is in flight - it is
the one place `VoxelEditBatch::Write`, `ModifyVoxel` and `ModifyVoxelFast` all
pass through - and `ChunkSystem::RepublishJournalledWrites` replays them
straight after the swap, **out of the CPU voxel rather than the recorded
colour**, shifted by the offset delta. `VOXAGINE_SYNC_AUDIT` goes **106-188 ->
0 of 75,497,472** over a run that destroys 48 k voxels, slides the window and
switches world. Three things not to undo: the journal is armed only on the path
that reaches a commit and is disarmed by cancellation, `~ChunkSystem`, `Resize`
and `ClearAll` (an id into a resized window names an unrelated place); a quiet
slide must republish **nothing**, which is gated as
`window-commit-writes-replayed` in `perf.txt`; and
`StreamingCounters::WindowCommitWritesLost` is the number that names the
defect - it is what the swap *would* have lost. **The first hypothesis was
`VoxelBaker::Clear` erasing an unowned voxel and it was wrong**;
`VoxelStampDivergingErases` measured zero on the run that had 124
disagreements, and that counter stayed because counting at the write is what
killed the theory in one run rather than in an argument.

**A world switch with a loading screen brings the new world up *behind* it.**
`WorldManager::LoadWorldAfterStreaming`/`UpdateStreamingWorld` (phase 8): the
target is initialized hidden, only its `ChunkSystem` and `RenderSystem` advance
- no scripts, AI, physics or input - and it replaces the loading screen in one
deferred transaction once it has arrived. Menu -> `Fishing_Village_Beat1`: the same
**~300 ms** `initialize` is now paid behind an animating loading screen rather
than in the one frame the player waits through, plus **1666-1717 ms** of
streaming across **2518-3716 drawn frames**, plus a **16.8-19.0 ms** activation.
Five things worth not undoing:

- **`RenderContext::ResizeWorldBuffer` takes the world being sized, not the
  visible one.** It used to ask the world manager for the top world, which is
  only ever right when those are the same world - and behind a loading screen
  they are not, so the resident voxel window was sized from a sprite-only menu
  world's grid and the level rendered **black**. Found on screen by Joey;
  `VOXAGINE_VOXEL_AUDIT` reporting 2.7 M active voxels on the black frame is
  what split "did not load" from "did not draw" in one run, and mean luminance
  over a headless capture (**1.73 -> 96.11**, against a direct load's 96.94) is
  what proved the fix.

- **Readiness is `IsInitialWindowReady() && !IsStreaming()`, and the far-field
  half is free** - 832/848/882 ms with it against 867/914 ms without, because
  the far field builds in 4 ms slices *alongside* the window. So the horizon is
  there at activation, which is phase 4's open judgement call closed.
- **Count frames and wall clock, and never divide one by a fps line.** The
  loading screen is sprite-only with a hidden world behind it, so it runs at
  2-3 thousand frames a second; 1400 frames read against the `[fps]` line's 81
  says seventeen seconds and the truth is under one. The frame count is the
  evidence the artwork animated, not a clock. (And two runs disagreeing 4x were
  this machine compiling in the background.)
- **`World::Unload(false)` is the loading screen leaving**, and it must not
  cancel the far-field build or clear the voxel window - the world behind it
  owns both. `RenderSystem::BeginWorldUnload` forgets every renderer's stamp
  rather than clearing it, on every path.
- **`AudioSystem::SetAutoPlayDeferred` gates `OnComponentAdded`, not just
  `Start`** - a hidden world keeps admitting roots, so that is the path its
  music would have come out of.

`IWorldStreamingReadiness` is how a check drives all of that with no render
context; it is a **test seam, not an abstraction layer**, exactly like
`IVoxelWindow`.

**A play session found ten defects in one afternoon, and nine were older than
the session.** Written up because the *shape* repeats: each was unreachable
until something else was fixed, so they surfaced in a chain rather than
independently.

- **`Weapon::Fire` and `Player::Catch` dereferenced the partner unguarded**, so
  a throw segfaulted whenever there was no second player. 2023 code, reachable
  only once a keyboard-only player could start a level alone.
- **The two players were never paired at all.** `GameManager::ResolvePlayers`
  cross-linked them on the *transition* into "both present" (`!bHadBoth &&
  both`) - a trigger that can be consumed without doing anything, and was.
  Everything in the throw/catch loop hangs off that one pointer, so the visible
  symptom was "the bullet never comes back", three layers away. **An edge
  trigger for a state that must hold is a bug; make it idempotent.**
- **`ResolveVoxelPreciseCollision` spun forever on a zero-extent collider**:
  `for (float z = -h.z; z <= h.z; z += h.z * 2.f)` steps by *nothing* when the
  extent is zero. Reached only by a body moving >5 units a tick - a thrown
  bullet - and in Debug every iteration submitted a debug line, so it looked
  like a hang in `DebugRenderer`. It counts to 8 now. Its inner raycast had a
  second, independent hang: `dist == 0.f` is exact equality, so a 1e-30 advance
  passed it forever.
- **`ChunkGrid::m_iGridLocks` was a plain `int`** incremented on the main thread
  and decremented from job-completion callbacks. A lost decrement lets the main
  thread rebuild the grid *while a job walks it*, which is a segfault on a node
  access with a perfectly valid `this`. Atomic now. The same four lines also
  captured a `std::vector` element by reference into a job and ran every group's
  `updatePaths` concurrently over the same nodes' `m_groupProperties` map.
- **Uninitialised `glm::quat`s.** `VoxelStampTransform::Rotation`,
  `VoxelStampPose::Rotation`, `BakeData::StampKey::Rotation` and
  `BakeData::LastRotation` were all default-constructed, which GLM leaves as
  stack garbage. It cost a CI failure that reproduced nowhere else - the
  runner's garbage was non-finite, this machine's was not, and the test printed
  two identical NaN lines as "diverged". They are explicit identities now, and
  **`Quaternion x;` in this tree is always a bug**.

**A scripted run destroys voxels now, and the token was never what was broken -
the script's *clock* was.** Chunk streaming phase 11. `StepUIScript` counted
display frames from process start, and a level spends hundreds of them with
gameplay held while its initial window streams, so the tokens were spent into a
world where no entity had ticked and `Player::Start` had not bound "Fire" to
anything. The keys arrived; there was no listener. It read as intermittent
because **how long the hold lasts is how long the bake takes**: 621 held ticks
in one Release run and 2,930 in the next, same binary, same command line. The
clock now stops while `World::IsGameplayHeld()` (`Keyboard::SetUIScriptPaused`,
set from `Application::Run`, asking *both* the top world and the streaming one
so a script cannot press keys at a loading screen either), and it prints
`[ui] script paused/resumed`. **Every run now ends with
`[destruction] N voxels destroyed, M protected, over B bursts`** from three
`StreamingCounters`; a run reporting 0 has not tested destruction whatever else
it did. Two lessons outlived the fix: the recorded diagnosis ("the input stops
between the synthetic key event and `InputHandler::BindAction`") came from
instrumenting `Weapon::Fire` only, when `VOXAGINE_GAMEPLAY_DEBUG=1` already
prints one line *upstream* of it and separates "no input" from "input, other
branch" in a single run - **when two hypotheses are both free, run both**. And
**a weapon firing is not a weapon hitting something**: the throw goes along
`Player::GetDirection()`, so on Beat2 from spawn the partner catches it and a
run with four perfect throws destroys nothing. Walk into Beat1's village first.

**The game is written as two roles, and one player used to hit a null in every
one of them.** `Weapon::Fire` dereferenced the linked player unguarded, so a solo
throw segfaulted; `Player::Catch` did the same one step later; and
`SpawnerManager`/`SpawnerEntity` started a wave only with both players in the
trigger box, so a solo arena closed its walls and never spawned anything. All of
it dates to the 2023 coursework and only became reachable when a keyboard-only
player could join and start a level alone. The rule now is **the receiver is the
reference player if there is one and the thrower otherwise**, applied in the
three places that decide it - `Player::Switch` (the role and the recall marker),
`Player::AddSpawnedBullet` (the incoming list Recall iterates) and
`Weapon::Fire` (the bullet's receiver). **The escape procedure is what makes
throwing to yourself work and must not be skipped**: it marks a bullet
un-escaped when the receiver is within catch range at the throw, so solo the
bullet has to fly clear before it can be caught - without it the thrower catches
their own bullet on the frame they threw it. `CameraMultiplayer`'s two-player
tests are deliberately untouched; framing two players is a different question.

**Player one is joined automatically** if `InputHandler::HasConnectedDevice`
says its device is present, and joining has a keyboard binding (`IK_J`, and a
`join` `--ui-script` token) for the first time - it was gamepad-Start and
mouse-click only, so a keyboard-only player could not leave the main menu and
no headless script could reach a level from the menus.

**A save is refused while chunks are streaming**, and that is a data-loss guard
rather than tidiness: `ChunkifyWorld` distributes the *live* entities into the
chunk grid, and mid-transition the live set is not the world - an incoming
chunk's roots may be staged but not admitted, an outgoing chunk's half
destroyed - and the result goes over the only copy of the level. It exists because the
`progressive-chunk-experiment` branch (3 commits, one ~4,700-line WIP dump)
attacked the chunk load/unload hitching with the right architecture — worker
back-buffer window build, atomic commit transaction, budgeted resumable
entity/encode/bake slices — but delivered it unreviewably, with two
memory-safety defects, a silently bundled renderer change
(`MARCH_STEP_BUDGET` 16384 → 1024 + occupancy-cell proxies) and per-manager
gameplay band-aids for a problem one ordering rule removes. The plan
re-derives it phase by phase on master; **the branch is a reference
implementation, do not rebase or merge it**. Both of the live master bugs its
ledger recorded are now fixed: `GameTimer` ran the fixed callback once per
display frame (phase 0), and `VoxelBrickGrid::FlushDirty` raced the chunk
worker's back-buffer writes (phase 1).

**Loading a chunk's entities is two questions, and only the second one is
observable.** `Chunk::StageEntityBatch` constructs roots *detached* - the
entity and its components exist and the world has not been told - so it runs
under `StreamingBudgets::EntityStaging` and starts **before the commit**,
from `US_RENDERING`, while the chunk worker owns the back buffer.
`AdmitStagedGameplay` then puts **every non-static root of the whole incoming
window in, in one frame** (`US_ADMITTING_GAMEPLAY`), and the static art
follows at `EntityAdmission` roots a frame. Three things follow:

- **The gameplay contract is the atomic non-static admission, and it is what
  replaces the experiment's per-manager polling** (`GameManager::
  ResolvePlayers`, `SpawnerManager::RefreshSpawnerLinks`, the `Weapon::Fire`
  guard - none of them land). Worst case measured over all 17 levels: 430
  non-static roots for a three-chunk slide. Admission is a pointer push per
  node; what scales with it is the stamp each renderer sets off next
  `PreTick`, which is phase 5's budget. **`MaxGameplayRootsPerAdmission` is
  the one `perf.txt` entry where a *drop* is the regression.**
- **`EntityAdmission` is a count, not a clock, and it is the only budget that
  has to be.** Every other budgeted loop pays as it goes; admission pays
  nothing now and everything next `PreTick`.
- **A staged root is the one raw `Entity*` held across a frame boundary, and
  its defense is ownership** - nothing else knows it exists. It leaves by
  admission (which nulls the slot) or by `ResetStreamingState`/`~Chunk`.

**A link expires only when its end cannot arrive, not when a timer runs out.**
`k_uiMaxWorldLinkRetries` is 240 frames, and a streamed level admits the entity
a link waits for whenever the player walks to its chunk - minutes, maybe. Every
`Spawners` and `AI Group` link in `Fishing_Village_Beat1` was abandoned four
seconds in with *"the source never arrived"*, because the source - the spawner
manager itself - is three chunks from the player's start. Eleven links, and an
arena wired to nothing; `grep -c 'Gave up connecting'` is 11 before and **0**
after. `World::SetKnownEntityIds` collects every id the `.wld` contains during
the load pass that already walks all of them, so the bound can ask *can this
end ever arrive?* - an id in a chunk waits indefinitely and costs nothing while
waiting (the retry is skipped entirely until streaming admits a root), an id the
level does not contain still expires on the frame budget. **Two shapes that
sound right and are not**: gating on `IsStreaming` (the window slides all through
play, so nothing ever expires) and spending the budget only when streaming is
idle (a world settles the moment the player stops walking, so standing still for
four seconds burns it anyway).

**A container link dangles unless something nulls it.** `JsonSerializer` recorded
scalar entity links for repair and *counted* container ones, on the stated
grounds that no shipped level had one. Beat1 has five - the spawner manager's
`Spawners` vector - invisible only because they never resolved. Making them
resolve made the dangling case live in the same session, and the crash was
**inside the guard**: `pSpawner->GetOwner()->IsDestroyed()` where the owner was
not null but freed. **A pointer you must dereference to validate cannot be
validated.** Container links are recorded and nulled now
(`ClearInstanceArrayElement`), *and* `SpawnerManager` reduces each link to an
entity id on the frame it is written and resolves ids from then on - the
`PlayerSlot` rule, applied to the one other place that held raw pointers to
streamed content.

**A link is an identity, not a pointer, and destroying an entity nulls the
links to it.** Two separate defects, both pre-existing, both now fixed:
`World::WorldConnectionInformation` held a raw `rttr::instance` of whatever
was being deserialized and dereferenced it a frame later to ask two questions
it can answer from `(entity id, component type)`; and `ResolveWorldLinks`
resolved once per `PreTick` and cleared the list, so a cross-chunk reference
one frame early became a permanent null. Links retry until both ends exist
(`World::k_uiMaxWorldLinkRetries`) and are then abandoned and counted.
**The worse one is M8**: a link, once made, is a raw `Entity*` that nothing
knows about, and chunk streaming destroys the target routinely - the reader
that bites is the serializer, which dereferences an `Entity*` property to
write its id when the *holder's* chunk unloads. `World::EntityLinkRecord` plus
`JsonSerializer::ClearEntityLinks` from `DeleteEntityFromLists` is the repair.
It was invisible for the life of the tree because **no engine type has a
reflected `Entity*` property** - every one is in game code, which the test
suite does not link. `Tests/Harness/StreamingProbe.h` is a test-binary entity
that has one, and ASan found the use-after-free on its first run.

**The link retry budget is counted in frames where gameplay runs, and counting
it in `PreTick`s made every link in the level expire before the level
started.** This is the "camera does not follow the player" defect, and it was
mis-attributed three times - to the editor's autosave, to a chunk unload
destroying the main camera, and to `CameraMultiplayer` caching the camera in
`Start` - because all three are real hazards in the same area and none of them
is this. `World::PreTick` runs throughout the initial-window hold (R1), and
`ResolveWorldLinks` is called from it, so the 240-attempt budget burns down
while **no entity has ticked even once**. A Debug `Fishing_Village_Beat1` holds
for 360-600 frames on this machine, so all eleven of the level's cross-chunk
links were abandoned before gameplay began: the camera's two players, the
pathfinding grid's centre entity, both weapons' player, the game manager's
player array and end positions. Release lifts the hold in under 240 frames and
works, which is the whole of why it read as intermittent - **four runs of the
same binary and command line split three to one on nothing but how long the
bake took.** `!pWorld.IsGameplayHeld()` gates the increment. Deliberately not
gated on `IsStreaming`, which slides constantly during play and would make the
bound never expire, which is what it exists to prevent.

**Measure it with `--map` plus the abandoned-link warning**, which is exact and
needs no screen: `grep -c 'Gave up connecting'` over a headless run was 11 in
three of four runs before and is 0 in six of six after, on two levels and in
both Debug and Release. The `[world] gameplay held N ticks` counter is the
other half - if N exceeds `k_uiMaxWorldLinkRetries`, every link in the level is
already gone.

**A player is referred to by index, never by name, pointer or discovery
order.** `Player::GetPlayerIndex` ("Player Index", derived in `Awake` from the
old `"Player"`/`"Player1"` names so shipped levels are unchanged) and
`PlayerSlot` (`Game/Source/General/PlayerSlot.h`) are the whole mechanism:
a slot that is empty re-resolves by index every tick, forever, so there is no
deadline to lose and a player destroyed by a chunk unload re-attaches when it
comes back. The serialized `Player 1`/`Player 2` links and the inspector still
work and are now an *override* rather than the mechanism. Three things it
replaced, all measurably unreliable: those links (above);
`GameManager::Awake`'s `FindEntitiesOfType<Player>()` indexed positionally
behind an exact `size() == 2` test, where the order is *admission* order and a
count of zero matched no branch and never retried; and `Player::Start` deciding
a player's role and animation set from `GetName() == "Player"`.
`GameManager::StartGame` also set `m_pPlayers[0]` persistent twice, so player
two was never pinned and could be unloaded mid-level.

**`VoxRenderer::SetFrame` does not mesh, and that is deliberate.**
`DYNAMIC_MODELS_PLAN.md` phase 1 called `ModelMeshStore::EnsureMeshed` there,
which is idempotent and cached and was still wrong, because `SetFrame` runs
while deserializing a chunk's roots: one `VoxRenderer` construction measured
**44.65 ms** greedy-meshing a model no model pass will ever draw
(`RenderSystem::Render` submits only non-static renderers, and it meshes
lazily itself).

**Stamping admitted renderers is budgeted, and the last thing between it and
the hitch gate is one model.** `RenderSystem::OnComponentAdded` used to write a
renderer's voxels inline as its component registered, inside `World::PreTick`;
it requests a stamp now and `VoxelBaker::Bake` does it under
`StreamingBudgets::VoxelBaking`. **Resumption needs no cursor**: a renderer the
budget did not reach keeps its `Updated`/`UpdateRequested` flags and the next
frame's scan finds it - so nothing crosses a frame boundary. Two rules came out
of that and both are load-bearing:

- **`m_bForcedUpdate` may only be cleared when the pass reached the end of the
  list.** A force is the *only* trigger a renderer past the stopping point has;
  clearing it early left a third of Beat2's voxels unwritten, with no symptom
  but missing geometry.
- **R1 waits for the stamps, not just the roots.** `IsInitialWindowReady` folds
  in `RenderSystem::HasPendingVoxelBakes`. Without it the player starts walking
  while the river bed is still being stamped, falls through the hole where it is
  going to be, and the level ends on the game-over screen. That is not
  hypothetical; it is how the defect was found.

**One renderer's stamp is split across frames, and the walk was never what lost
the geometry.** Chunk streaming phase 9. `ForEachStampedVoxelRange`
(`VoxelStamp.h`) carries the loop counters *and* the duplicate-suppression
position on a `VoxelStampCursor`, and `ForEachStampedVoxel` is that same
function with no budget - one loop rather than two that have to agree. The first
attempt at this lost about 580 k voxels and was reverted; re-derived with the
oracle built in, the sliced walk is **identical to the unbounded one at every
budget from 1 upward**, over six poses and two real models
(`Tests/Rendering/VoxelStampChecks.cpp`, using `Tests/Harness/VoxModelFile` to
read a `.vox` with no resource stack). Three rules hold it together and each one
is a way the first attempt could have failed:

- **A half-stamped renderer must not be cleared again.** `Clear` erases by
  recorded position, and failing that by owner slot over the whole box, so it
  reaches the part already written - and the resumed walk never goes back for
  it. A renderer interrupted *n* times would keep only its last slice, which is
  a deficit that grows as the slices get finer. `bResuming` gates the clear, the
  transform resets and every skip test in `Bake`.
- **The bake bookkeeping is written at the *start* of a stamp.** `Positions`,
  `Size`, `Generation`, the stamp key and the stamped box describe what is in
  the buffer *now*, mid-stamp included, so a partial stamp can be cleared,
  abandoned or resumed by the code that handles a whole one.
- **A partial the ground moved under is restarted, not continued** - buffer
  rebuilt, window slid or stamp key changed. `StreamingCounters::
  VoxelStampRestarts` counts it and it should stay small.

**Measure it settled, or the measurement lies in exactly the shape you are
looking for.** Bounding a stamp moves work into more frames, so a sliced run
reaches any given instant with *less* of the level written: six seconds into
`Fishing_Village_Beat2` the sliced counts are over a million short and the
deficit scales with slice size - the precise signature of lost geometry. Settled
they are **4,930,065 active voxels at every slice size, unsliced included**, to
the voxel, over eight runs. `[voxel-audit]` now says `- STILL STREAMING, this
count is not settled` when `ChunkSystem::IsStreaming()`, and
`VOXAGINE_STAMP_SAMPLES` sweeps the slice size without a rebuild. The cost is
the wait: `[world] gameplay held` goes **728-779 -> 910-1,071 ticks** at the
shipping 8,192 samples, and the knob for that is `StreamingBudgets::VoxelBaking`
(2 ms), not the sample count.

**A world's first window streams like any other, and `World::Initialize` is
876 -> 317 ms because of it.** `ChunkSystem::Start` used to decode nine chunks,
deserialize every root and stamp every static renderer synchronously, off the
frame loop where nothing can animate over it; it queues an ordinary
`ChunkUpdateGroup` now. The chunk the camera starts in is already resident
regardless - `JsonSerializer::DeserializeWorld` loads `CameraChunkIndex` while
it is still building the world - so the player never stands on nothing.
**The far field builds in 4 ms slices from `ChunkSystem::Tick`** and reports
itself *unbuilt* for the whole build, which is what makes a partial volume safe:
every shader reads a zero grid size as "no far field", so neither a half-filled
volume nor the previous level's is ever sampled. Its model pins are released on
completion *and* on cancel, and `World::Unload` cancels.
**What is left of `initialize` is one thing: `RenderSystem::Start`, 258 ms of
`ResizeWorldBuffer`.** The loading-screen flow itself (`LoadWorldAfterStreaming`)
is *not* landed - see the phase 4 notes for the full list of what is owed.

**Gameplay does not tick against a missing initial window** -
`World::IsGameplayHeld()`, from `ChunkSystem::IsInitialWindowReady()`. It is
live as of phase 4: a world now spends real frames with no resident window and
gameplay advances through none of them. That one rule is what replaces
`Bootstrap` metadata,
`Player::SetPersistent` in the constructor and the `PhysicsBody` freeze.

**`Application::Run` reports its own stages now** (`CPU Frame PreTick/Tick/
FixedTick/PostTick/Render/Present/...`, under `VOXAGINE_PROFILE=1`). Nothing
in this tree said where a *frame's* time went - every named cost was inside
one system - so a 94 ms transition frame with no streaming timer above 8 ms in
it was unattributable. It is two `VoxelBaker` stamps: `World::Render`'s `Bake`
(54.7 ms) and `PreTick`'s `OnComponentAdded` (34.3 ms), both phase 5's.

**A window slide publishes once, in one transaction, and everything else is
around it.** `ChunkUpdateGroup`'s `US_COMMIT` is the only place physics
volumes, world offset, buffer swap and camera move, and they move together
with nothing between them that can yield; `ChunkSystem::CommitWindow` is the
whole of it and costs 0.002 ms. The worker builds the *entire* incoming window
into the mapper's back buffer and rebuilds that buffer's occupancy pyramid
itself (`VoxelBrickGrid::FlushDirtyBackBuffer`) — so `FlushDirty` on the main
thread walks the **front buffer only**, and a back-buffer flush from anywhere
else is counted and asserted. Group advance is once per *display* frame, from
`ChunkSystem::Tick`, one state per frame; `FixedTick` only creates groups.
Entity loading runs after the commit and is still unbounded — 44 ms a
transition, which with the admitted renderers' stamps (17.6 ms) is the *whole*
of the remaining ~102 ms hitch and is phases 3 and 5's to remove.

**Unloading a chunk is two budgeted main-thread states, and encode is no
longer a job.** `US_START_UNLOADING` serializes the departing roots
(`Chunk::PrepareUnloadBatch`) and `US_ENCODING` RLE-encodes the voxels
(`EncodeVoxelBatch`), both under `StreamingBudgets` — 0.37 ms and 2.00 ms peak
respectively, against 2.70 ms and a 9–12 ms worker blob before. Three things
follow that are easy to undo by accident:

- **No `Entity*` crosses a frame boundary.** The chunk's entities are
  re-discovered every slice (0.06 ms) and a root is serialized *whole* inside
  one slice. That is the defense against the experiment's ledger E1, and it is
  why the branch's resumable post-order stack is deliberately not here: the
  largest root hierarchy in any shipped level is 68 nodes against a 1.10 ms
  whole-chunk serialization, so bounding below a root buys microseconds and
  costs exactly the lifetime hazard it would be replacing.
- **`ChunkSystem`'s storage pool is what makes encode affordable here.** A
  resident chunk is 48 MiB; encode clears logical sizes only and the block
  moves to the pool for the next arriving chunk, so a settled slide allocates
  nothing. Six blocks, cleared with the world.
- **Spreading the unload across frames buys frame time and spends transition
  latency** — about 230 ms before a group drains and the next may start. Fine
  because a chunk is 256 units across; `ChunkUpdateGroup::MillisecondsSinceCreated`
  is the number if that changes.

**Destroyed terrain came back on chunk reload, and the codec was innocent.**
A chunk re-serializes its departing roots from the *live* reflection
registration, so its in-memory JSON gains properties the level on disk never
had — and `VoxRenderer::SetEmissive` requested a bake update unconditionally.
`VoxelBaker::Bake` ands `IsChunkInstanceLoaded()` with `!UpdateRequested()`,
so that request walked past both guards and stamped the pristine model over
voxels decoded with their damage in them. `SetChunkInstanceLoaded(true)` now
clears the "something changed" flags — the one point that knows the decoded
voxels are authoritative — and `StreamingCounters::ChunkInstanceRestamps` must
stay zero. **Do not re-derive the encode-vs-`Clear` ordering-race theory**; it
was measured and the damage survives the codec exactly.

**A departing renderer must not clear its stamp.** `US_COMMIT` runs two states
earlier, so its recorded positions name whatever slid in underneath; `Clear`
declines to erase a cell another renderer owns, which covers models and covers
neither the ground row nor settled debris. `Chunk::PrepareUnloadBatch` marks
each one `MarkChunkUnloading()` and `RenderSystem::OnComponentDestroyed` calls
`VoxelBaker::ForgetChunkStamp` instead.

**`IVoxelWindow` (`Core/Voxels/VoxelWindow.h`) is a test seam, not an
abstraction layer.** `RenderContext` implements it over its voxel mapper;
`StreamingHarness` implements it over two `std::vector<uint32_t>`s. That plus
`World::PreLoad(false)` — a world with no `RenderSystem` — is what lets the
real `ChunkSystem`, real `Chunk`s and the real `JsonSerializer` run in
`voxagine_tests` with no GPU. Nothing else should implement it. Ask
`World::GetRenderContext()` rather than chaining Application → Platform: a
null render context is an ordinary state now, and `Camera`, `Canvas` and
`LoggingSystem` each used to crash on it rather than degrade.

**Streaming has two shared files worth knowing about.**
`Core/ECS/Systems/Chunk/StreamingBudgets.h` holds every bound in one place,
injectable as a unit count so a test can single-step a resumable loop; a
budget spelled `Unbounded()` is the honest name for work no phase has made
resumable yet, and grepping for it finds what is left.
`StreamingCounters.h` holds exact, machine-independent counts — commits per
group, chunk regions written, roots per entity pass, roots and encode runs per
slice, chunk storage reused against allocated, and the invariants that must
stay zero — which `Tests/Baselines/perf.txt` gates. A streaming benchmark sets
*unit* budgets so its per-slice counters are exact numbers rather than a
function of how fast the machine is; `StreamingBudgetOverride` is the RAII for
that and `Units(1)` is the single-step sweep.

**Three streaming defects that were pre-existing on master and are now fixed,
all found by phase 2's fuzz and failure injection.** Worth knowing because
each is a class rather than an incident: unloading a chunk that was never
loaded wrote an empty root list and an empty RLE stream over the level's only
copy of both (skipped and counted now); a window sliding over the main camera
serialized and *destroyed* it, after which everything read it freed (the world
nulls its own pointer now and `ChunkSystem` checks — the same move
`GetRenderContext` made); and the deserializer indexed `"Children"` and
`"Components"` without asking whether they exist, which rapidjson turns into
an abort in Debug. Streamed content is data, and data is hostile.

**Packaging the game for iOS/Android has its own plan: `Docs/MOBILE_PORT_PLAN.md`,
and all nine of its phases have been worked — see `Docs/MOBILE_PORT_LOG.md`, which
is the file to read.** Same phased format. **Does not cover an iPad-native
editor** — that and a planned ImGui-replacement UI system are separate future
initiatives, deliberately kept out of that plan.

What that work landed, in one paragraph each, because it changed things every
future session touches:

- **Audio works for the first time.** miniaudio 0.11.25 (vendored, public
  domain) replaced FMOD, which was never linkable. `VOXAGINE_AUDIO_BACKEND`
  is `MINIAUDIO` or `NONE`; `AA_NONE` still selects `NullAudioContext` at
  runtime. **`VOXAGINE_AUDIO_NULL_DEVICE=1` loads, decodes and mixes to
  nothing** — use it for every headless run, unlike `AA_NONE` it exercises the
  audio path. Music streams, effects decode; the split is the `_BGM` naming
  convention that already existed.
- **Vulkan is loaded at runtime through volk, not linked.** `VulkanAPI.h` is
  the only file that includes `vulkan.h`; everything else includes it. This is
  not tidiness — **Android's `libvulkan.so` exports nothing past 1.1** and this
  renderer is core-1.3 sync2 and dynamic rendering, so a direct link leaves
  five undefined symbols. `volkInitialize`/`volkLoadInstance`/`volkLoadDevice`
  are in `VKDevice`. Anything Vulkan called before `CreateInstance` is a null
  function pointer.
- **The editor is no longer compiled into the game.** `VOXAGINE_EDITOR_SOURCES`
  in `CMake/EngineSources.cmake` — `Editor.cpp` has no `#ifdef EDITOR` around
  it, so a game build used to carry the whole editor and, through
  `FileBrowser`, the desktop file dialog. Four `Editor/` files still build
  everywhere and the comment there says which and why.
- **`CMake/Platforms.cmake` is where platform questions are answered once**:
  `VOXAGINE_MOBILE`/`ANDROID`/`IOS`, the Vulkan 1.3 floor, and the min API
  decision (Android 31, iOS 16.0). `CMake/SDL3.cmake` builds SDL3 from source
  when cross-compiling and uses `find_package` on desktop.
- **`android-arm64` and `android-arm64-release` presets build and link**
  against NDK r27c, and CI has a compile-only Android lane that asserts no
  undefined Vulkan symbols. **iOS is plumbed and completely unverified** —
  it needs macOS.
- **Nothing has run on a device.** Not once. Every "on a real phone"
  acceptance in that plan is open, and `Docs/MOBILE_PORT_LOG.md` says exactly which.

## Environment

- **git + GitHub** (PRs), not Perforce.
- `gh` CLI lives at `~/.local/bin/gh`, authenticated as `joey-j7`.
- The Vulkan backend and bring-up target **build and run locally on Linux**.
  The engine library does not build yet — see "Port status" below. MSBuild,
  the `.sln` and every `.vcxproj` are gone; DX12 is deleted.
- **The dev machine: RTX 4070 SUPER, one 2732x2048 display at 60 Hz, Hyprland
  on Wayland.** All three mattered for a real bug. 60 Hz against a 200 fps
  engine is what produced the presentation judder; Hyprland tiles the window
  (1365x2003, *portrait*) and ignores `Fullscreen` until Alt+Up, so "fullscreen"
  here is a borderless tile and a 16:9 locked target is letterboxed hard.
  Do not assume a 16:9 window or that fullscreen means exclusive.

## Build & CI

**Everything goes through `CMakePresets.json`.** Five desktop presets - `game`,
`game-release`, `editor`, `editor-release`, `bringup` - plus the mobile ones
below, each writing to a nested `Build/<Platform>/<Type>/<Debug|Release>/`
(`Build/Linux/Game/Debug`, `Build/Android/Game/Release`, `Build/iOS/Game/Debug`,
...) rather than a flat `Build/<preset>/`. The desktop presets' platform segment
is `${hostSystemName}`, a CMake preset macro that expands to
`CMAKE_HOST_SYSTEM_NAME` at parse time - `Linux` here, `Windows` there, no
per-OS preset needed. It would read `Darwin`, not `MacOS`, if one of these ever
configured on a Mac - that's CMake's own name for it. `Build/` is the only
build directory in the root and the only one `.gitignore` needs.

```bash
cmake --preset bringup && cmake --build --preset bringup
./Build/Linux/Bringup/Debug/bin/voxagine_bringup --frames 120
```

The presets exist because the raw option combinations are not obvious and
getting them wrong fails quietly rather than loudly:

- `VOXAGINE_BUILD_ENGINE` defaults **off**, so a plain `cmake -S . -B build`
  produces no `BitBuster` target at all - which is what an IDE opening the root
  `CMakeLists.txt` used to show, one bring-up executable and no way to build
  the game.
- `VOXAGINE_BUILD_BRINGUP` defaults **on**, so every one of those directories
  also built `voxagine_bringup` and its `bin/` held two executables.
- Rider (and CLion) read `CMakePresets.json` directly and offer the five as
  profiles. Without them they default to `cmake-build-<profile>/` with stock
  options, which is the failure above.

**What is and is not committed under `.idea/`.** Run configurations are - one
per preset, and each sets the working directory to `Game/`, because BitBuster
resolves its assets relative to the current directory and launching it from the
repo root just fails to find anything.

**A run configuration names a target *and* a profile, and #54 broke the pair.**
Making the editor its own app bundle renamed the editor executable target
`BitBuster` -> `VoxagineEditor` (the file is still `bin/Voxagine`), and nothing
updated the two Editor run configurations. `BitBuster` does not exist in the
editor profile at all, so Rider could not resolve (editor, BitBuster) and
rewrote `CONFIG_NAME` to `game` - the profile where that target does exist - on
every load. It rewrites the file *in place*, so the damage lands in `git diff`
and gets committed by anyone who is not looking (#55 carried it to master).

The symptom is not that the configuration disappears: it stays listed and stays
marked visible in Edit Configurations, and drops out of the dropdown because
Editor, Editor (Release) and Game now all resolve to the same
(game, BitBuster) pair and Rider will not offer three identical entries. If a
run configuration keeps reverting, check its `TARGET_NAME` against the targets
its profile actually builds - `ls Build/<...>/.cmake/api/v1/reply/target-*.json`
is the list - before assuming Rider is at fault. And `git diff .idea/` before
every commit that touches it.

**The `<preset> - <preset>` profiles are not corruption.** `CMakePresets.json`
has a `buildPresets` entry per `configurePreset` with the same name, and Rider
makes a profile per (configure, build) pair and names it that way. They are
disabled, they cost nothing, and deleting them from `workspace.xml` only means
they come back on the next reload. Do not chase them. `workspace.xml` is not: it is where the
IDE keeps its *own* copy of the CMake profiles, alongside window layout and open
tabs, and JetBrains treats it as per-user. Do not be tempted to commit it to
share build configurations - that is `CMakePresets.json`'s job, and the IDE
derives its profiles from it. Note that Rider does not replace stale profiles
when a preset's `binaryDir` changes, it adds new ones next to them, so a rename
there leaves duplicates in everyone's dropdown that only they can clear.

**Release is a real configuration, not a theoretical one.** `_DEBUG` is defined
only for Debug (`add_compile_definitions($<$<CONFIG:Debug>:_DEBUG>)`), and
`VKRenderContext::InitializeBackend` gates the Vulkan validation layers on it -
so Release is the build to measure frame times in, and Debug is the one that
catches API misuse. Release went unbuilt long enough to rot once already: `Box::
Intersects` was declared `inline` in the header but defined in `Box.cpp`, so
only `Box.cpp` could call it. Debug linked because at `-O0` GCC still emits an
out-of-line copy; Release did not, because at `-O2` it inlines every use inside
`Box.cpp` and emits nothing. Build both.

- **`enable_testing()`, never `include(CTest)`.** The module is CDash machinery
  and adds twenty-eight custom targets - `Continuous*`, `Nightly*`,
  `Experimental*` - for a dashboard this project does not have. They are not
  free: an IDE reading the CMake model makes a run configuration out of every
  one, and thirty-seven entries is how the four real ones stop being findable.
- Options: `VOXAGINE_BUILD_BRINGUP` (on), `VOXAGINE_BUILD_ENGINE` (off),
  `VOXAGINE_CHECK_RTTR` (off, on in CI), `VOXAGINE_ENABLE_FMOD` / `_OPTICK` /
  `_NFD` (all off). RTTR is only pulled in when the engine or the reflection
  check is enabled, so the default build stays offline-capable.
- Engine translation units are listed explicitly in
  `CMake/EngineSources.cmake`, not globbed — during the port a glob would
  silently pick up half-ported files.
- CI (`.github/workflows/build.yml`) configures into `Build/ci` rather than via
  a preset, because it matrixes `CMAKE_BUILD_TYPE`. It builds Debug and Release
  on `ubuntu-latest` and runs `ctest`. **It now builds the engine, the game,
  the bring-up target and the tests** — `VOXAGINE_BUILD_ENGINE` was off there
  for the whole port, so the only C++ it compiled was `VulkanBringup.cpp` plus
  `Core/Settings.cpp`, and the engine, game and editor had no automated cover
  at all. To reproduce CI exactly:

  ```bash
  cmake -S . -B Build/ci -G Ninja -DCMAKE_BUILD_TYPE=Release -DVOXAGINE_BUILD_ENGINE=ON -DVOXAGINE_BUILD_BRINGUP=ON -DVOXAGINE_BUILD_TESTS=ON && cmake --build Build/ci && ctest --test-dir Build/ci
  ```

  **There is a second lane now: Android, compile-only, Debug and Release.** It
  cross-builds `libmain.so` for arm64 against the runner's NDK and asserts it
  has no undefined Vulkan symbols - which is the failure Android's 1.1-only
  loader produces and which the Linux lane cannot see. Reproduce it with the
  `android-arm64-release` preset (needs `ANDROID_NDK_HOME`). There is no iOS
  lane and no Gradle lane: neither has ever run anywhere, and a job that has
  never passed once is a red mark rather than a check.

  **Still build the `bringup` preset before pushing anything under `Vulkan/`**
  if you are iterating locally — the four editor/game presets all skip
  `VulkanBringup.cpp`, so a changed `VKSwapchain` signature is green under them
  and red in CI.

  **The runner has no GPU, no display and no Vulkan SDK**, so `dxc` is absent
  and the shader target is skipped with a warning — CI proves things compile,
  link and pass their CPU tests, not that they render. (`add_dependencies` on
  the skipped shader target is guarded for exactly this reason; unguarded it is
  a generate-time error and it is what stopped the engine being buildable
  there.) A lavapipe + `SDL_VIDEODRIVER=offscreen` job would close the
  presentation gap.
- Ubuntu has no SDL3 package yet, so CI builds SDL3 from the `release-3.2.0`
  tag. Locally, distro SDL3 is fine (found via `find_package`, falling back to
  pkg-config).
- **`VOXAGINE_BUILD_TESTS` builds `voxagine_tests`**, the whole suite as one
  binary. Off by default, on in CI, and it errors if `VOXAGINE_BUILD_ENGINE` is
  off. Sources are listed explicitly in `CMake/TestSources.cmake`. There is no
  GTest dependency any more. **`Tests/README.md` is the file to read before
  adding a test**; the short version is below under "One test system".

  ```bash
  cmake --preset editor-release -DVOXAGINE_BUILD_TESTS=ON
  cmake --build --preset editor-release
  ctest --test-dir Build/Linux/Editor/Release --output-on-failure
  ```

## Windows is still a target

The port is to Vulkan, not away from Windows. Keep the tree building for both:
platform-specific code goes behind `_WIN32` or a CMake `if(WIN32)`, never in
the shared path. **`_WINDOWS` is not defined by this build** - it came from the
deleted `.vcxproj` files, so any `#ifdef _WINDOWS` is dead code, and the three
that survived were still calling into deleted DX12 heap managers.

What makes it portable today: the renderer is Vulkan-only, SDL3 covers window,
input and gamepads, `PosixFileSystem` is plain C stdio despite the name, and
`teenypath.cpp` is `std::filesystem`. The only genuinely per-platform piece is
the editor's file dialog - `nfd_win32.cpp` and `nfd_portal.cpp`, selected in
`CMake/EngineSources.cmake`.

**`nfd_portal.cpp` includes `<SDL3/SDL.h>`,** which is the one thing under
`Source/External/` that knows about SDL. It is not upstream vendored code - it
was written for the port - but the dependency is deliberate and load-bearing:
the dialog shells out to zenity or kdialog and the main thread has to wait for
the child, and a main thread parked in `read()` stops answering the
compositor's xdg-shell pings, so the desktop puts up "application is not
responding" over a perfectly healthy editor. It polls the pipe on a 16 ms
timeout and calls `SDL_PumpEvents` each pass; that is what sends the pong.
Don't "clean up" the include. The window does still hold its last frame while
the dialog is open - fixing that means making `FileBrowser::OpenFile`
asynchronous, which its callers do not expect. `nfd_win32.cpp` is untouched and
still blocks.

**It has not actually been compiled on Windows since the port began**, so treat
it as maintained-by-construction rather than verified. A Windows CI job would
be the cheap way to keep it honest; `.github/workflows/build.yml` is Ubuntu
only.

## Linux/Vulkan port — agreed plan

- **Single Vulkan renderer. DX12 is deleted outright**, not kept as a parallel
  backend — the perf difference doesn't justify maintaining two.
- **SDL3** for windowing/input, **CMake** for the build.
- **HLSL shaders are kept** and compiled with **DXC `-spirv`**, wired into the
  build via `CMake/Shaders.cmake`. All 15 of the game's entry-point shaders
  compile and pass `spirv-val` **with no source changes** - the expected
  `[[vk::binding(x,y)]]` pass was not needed.
  **The register shifts are load-bearing.** HLSL's b/t/u/s are four separate
  register namespaces; Vulkan has one binding namespace per set. Without
  `-fvk-*-shift`, `VoxelRenderer.ps.hlsl` puts its `b0` cbuffer and its `u0`
  buffer both at set 0 binding 0, which is an invalid descriptor set layout -
  and it compiles clean, so nothing catches it until pipeline creation. The
  shifts in `CMake/Shaders.cmake` must stay in sync with `VKBindings` in
  `Vulkan/VKShaderBindings.h`; that pair is the SPIR-V/C++ contract.
- First milestone: **boots into a window with a Vulkan clear screen**. Done, and
  long since passed.
- **`-fvk-use-dx-layout` is load-bearing too.** The engine memcpys tightly
  packed C++ structs straight into structured buffers, which is D3D's layout.
  Without this DXC pads `float3` members to std430, giving
  `StructuredVoxelBuffer` a 48-byte stride where the C++ side writes 32, so
  every element after `[0]` is read from the wrong offset. It also needs
  `scalarBlockLayout` enabled on the device, because the packed `float3`s then
  straddle 16-byte boundaries.

## Port status

**The game runs.** BitBuster boots, renders the voxel world, sprites, ImGui
text and the debug lines, plays at ~200 fps, resizes, and closes cleanly, with
**zero validation errors**.

Done: DX12, the dead `_OPENGL`/EGL/WGL backends, ORBIS remnants, `PS4/`, the
Windows `.lib` blobs and all MSBuild files are deleted. `RenderDefines.h` names
no graphics API. RTTR comes from upstream v0.9.6 via `CMake/RTTR.cmake`. SDL3
replaces DirectXTK for window, keyboard, mouse and gamepad. Audio runs silent
through `NullAudioContext` unless `VOXAGINE_ENABLE_FMOD` is set. The whole
Vulkan backend exists: device, swapchain, allocator, upload buffer, descriptor
layouts, command engine, render and compute passes, and all five `Objects/`.

```bash
cmake --preset editor && cmake --build --preset editor
cd Game && ../Build/Linux/Editor/Debug/bin/BitBuster      # must run from Game/, assets are relative
```

Use the presets rather than raw `-B` invocations - see "Build layout" above for
why, and swap `editor` for `game`, `editor-release` or `game-release`.

**The editor runs.** It boots, renders its UI, loads worlds and shuts down
cleanly. **The "zero validation errors" that used to be claimed here is stale**:
it now emits 18 lines of `VUID-RuntimeSpirv-NonWritable-06340` per run - a
fragment-stage storage buffer that is written without `fragmentStoresAndAtomics`
enabled, which is `VoxelRenderer.ps.hlsl`'s `u0`. Verified identical on
`origin/master`, so it is pre-existing and nobody's most recent change; whether
the game build emits it too has not been checked. **The reason it went unseen
is worth more than the defect**: #54 renamed the editor's executable target
`BitBuster` -> `VoxagineEditor` (output `bin/Voxagine`), and the orphaned
pre-rename `bin/BitBuster` sat in the editor build directories for weeks, so
every "run the editor" - by hand, by script, and by an agent verifying a
branch - was running a stale binary that predated whatever it was checking.
Delete an app target's old artifact when you rename it. Earlier notes here described it as
hanging during bulk asset loading; it was crashing. The mid-line truncation
that looked like a wedge is just block-buffered stdout losing its last buffer
when a process dies on a signal - run it under `gdb -nx -batch -ex run` and the
signal and stack arrive immediately. Nothing about it was per-texture or
threading, so those leads are gone.

Five separate bugs, in the order they surfaced:

1. **`TextureReference::Descriptor` was never set by anything.** A DX12 GPU
   descriptor handle with no Vulkan counterpart, and the editor read it as
   `*(ImTextureID*)ref->Descriptor` - an unconditional null deref on the first
   frame that drew the menu bar. The field is gone; `TextureView` is the
   `ImTextureID` now.
2. **`VKImContext` ignored `ImTextureID`.** One descriptor set per frame,
   always the font atlas, so every `ImGui::Image` would have sampled font
   pixels even without the crash. It resolves each draw command's texture to
   its `View` and binds a set per distinct texture, pooled per frame slot. The
   font atlas goes through `VKTextureManager::CreateTexture` so the font and a
   sprite resolve identically.
3. **`Editor::m_pSelectedEntity` was uninitialised** and `HasSelectedEntity()`
   is a null test, so the inspector walked a garbage `Entity*` on frame one.
4. **`NullSoundReference::Load` returned true without setting `m_bIsLoaded`.**
   Its own comment says callers should treat sounds as present, but
   `IsLoaded()` reads the flag - so in a silent build *every* sound reported as
   a failed load.
5. **`EditorReferenceManager::ImportFiles` deleted references it did not own.**
   Its failure branch did `delete RefResource` on an object still held by a
   `ReferenceManager` under its path, leaving every `.ogg` key pointing at
   freed memory; the first entity naming one crashed in `AddReference`. It
   calls `Release()` now. **This is the one worth remembering** - it fires for
   any resource that legitimately fails to load, not just sounds.

Also fixed while verifying: `VKRenderContext::Deinitialize` destroyed the
device, but the command engines are `RenderContext` members and a base's
members are destroyed *after* the derived destructor body - so their mapped
upload pages unmapped against a dead device, which the loader turns into an
abort rather than an error. Deinitialize now releases the base's GPU objects in
dependency order first.

**The 16:9 lock is the game's, not the editor's.** `Settings` defaults
`m_fLockedAspectRatio` to 16/9 for everything, and `Application::Run` clears it
under `EDITOR`. Related: two places normalised the mouse against the *window*
while the image is the constrained render target, which `VKSwapchain` blits
into the **centre** - `ImguiSystem::Update` and `Camera::ScreenToWorld`, so
both ImGui hit tests and the viewport picking ray were out by the letterbox.
Both go through `RenderContext::WindowToRenderNormalized` now.

**The present blit fits with integer maths.** It used to compare the two aspect
ratios as floats and divide by one of them, which loses the exact fit for about
one window size in twenty-five - 640x599 fitted to 640x598 - and a one-pixel
mismatch turns the whole present into a linear resample. It reads as a slightly
soft image rather than as a bug, so `VKSwapchain` now logs once if it ever does
rescale.

**Autosave defaults to every 5 seconds** (`Game/UserSettings.vguser`, and
`UserSettings::InitializeDefaultSettings` hardcodes the same), which is a full
world serialization at that interval. Worse, a world with no file yet - New
World, never saved - has an empty autosave path, `SerializeWorldToFile` rejects
it, and it logged "Auto-Save world failed!" every interval forever.
`AutoSaveWorld` returns early with no path and won't re-enter while a save is
in flight; the interval itself is still 5 seconds and is data, not code.

**The window asks for `SDL_WINDOW_HIGH_PIXEL_DENSITY`.** Without it, a display
at a fractional scale (1.334 here) hands SDL a logical-size surface and the
compositor stretches it to real pixels - a non-integer upscale of the whole
frame, invisible on the voxel world and brutal on 13px UI text. The catch is
that `SDL_GetMouseState` then reports *logical* units while everything else is
pixels, so both readers go through
`SDLWindowContext::GetMousePositionInPixels`.

## The double-gamma present

The frame was washed out and desaturated for the whole life of the port —
noticeably lighter and flatter than the original footage, and it hit **ImGui
and the editor chrome too**, which is the clue that says the cause is after
compositing rather than in any one pass.

`VKSwapchain` chose a `B8G8R8A8_SRGB` surface format, under a comment claiming
`R_DEF_RESOURCE_FORMAT` was `E_R8G8B8A8_UNORM_SRGB`. It is `E_R8G8B8A8_UNORM`
and always was. Every pass renders into that format holding values that are
**already gamma encoded** — the art is authored in sRGB, lighting is applied
directly to it, and `AmbientOcclusion.hlsl` hand-applies `pow(x, 2.2)`
precisely because the pipeline is gamma-space. Present is a `vkCmdBlitImage`
from that target into the swapchain image, and **a blit converts formats**: an
sRGB destination treats the source value as linear and encodes it. So the frame
was encoded a second time and 0.5 presented as 0.86. D3D12 presented from a
UNORM back buffer, so this arrived with the port.

The swapchain now requires an 8-bit UNORM format and says so loudly if the
surface offers none. **This is not the linear-light pipeline**, and it stayed
correct when that landed: `Docs/RENDERING_PLAN.md` 7.2 made *lighting* linear but
kept the encode at the end of scene shading, so the present is still a straight
copy of bytes the shaders already encoded and a *third* encode is still what
adding an sRGB swapchain back would cost. See "Lighting is linear" below.

Worth generalising: any format conversion at the blit is invisible in code
review and looks like an art problem on screen. If colours drift again, check
what the source and destination formats of the present blit actually are before
touching a shader.

## Lighting is linear; storage and compositing are not

`Docs/RENDERING_PLAN.md` 7.2. `Color.hlsl` holds the transfer functions. The linear
region runs from `SrgbToLinear` on albedo inside `ShadeSurface` to
`EncodeSceneColor` at its four callers — the two `VoxelRenderer` pixel shaders,
`FarField.hlsl`'s `ShadeFarField` and `Particles.vs.hlsl`. Grep for
`EncodeSceneColor` and you have the whole boundary; **a new shading site that
forgets it writes linear values into an 8-bit target and reads as blown out.**

Everything downstream is unchanged and deliberately so: the targets are
`R8G8B8A8_UNORM`, FXAA and the UI composite work on encoded values because both
are correct there, and the present is a byte copy. That is not the sRGB
swapchain the plan originally described — read 7.2's notes before "finishing"
it.

**The constant that is easy to get wrong.** Every light colour in
`Lighting.hlsl` converted cleanly, but `SKY_AO_CURVE` had to go 1.35 → 2.97 and
the shine line needed `GammaGainToLinear`. A multiplier tuned against an encoded
image is not a linear multiplier: applied to radiance that is encoded afterwards,
an exponent of 1.35 arrives as ≈0.61. AO does not break, it quietly weakens,
which nothing catches.

## What the black screen turned out to be

Three unrelated bugs, none of them the render pass instance the earlier notes
suspected. Worth reading before chasing a similar ghost:

1. **`ChronoGameTimer` threw away almost all elapsed time.** It converted each
   delta to canonical ticks with truncating integer division while always
   advancing `m_LastTime`, so a loop spinning faster than one 100 ns tick lost
   nearly every fraction. The frame limiter fired about once a second, so the
   fade-from-black took a minute of wall time and the whole engine *looked*
   frozen at 1 fps. It now carries the conversion remainder.
2. **The interleaved `Begin` order** (below) meant the UI and Debug passes
   never opened a render pass instance.
3. **The structured buffer stride mismatch** (`-fvk-use-dx-layout`, above) fed
   the voxel pass garbage AABBs, so it rasterized nothing.

Two conventions the D3D-era shaders depend on, now set in `VKRenderPass`: a
**negative-height viewport** (Vulkan's Y-down clip space back to D3D's Y-up)
and a **clockwise front face** to match. ImGui deliberately does the opposite -
it works in Y-down pixels already, so `VKImContext` uses a positive height.

**Blast radius, measured:** of 387 non-vendored source files only **34** name a
`P*` alias, and 31 of those are inside `Core/Platform/Rendering/`. The outliers
are `PlatformData.h`, `VoxModel.h` and the ImGui contexts. ECS, editor and game
logic never touch them — earlier notes here called this "engine-wide", which
overstated it.

**Already cleanly abstracted** — add-a-subclass work, not rewrites:
`WindowContext`, `InputContext`, `GameTimer`, `AudioContext`, all selected via
`PlatformType`/`RenderingAPI` in `Platform::Initialize()`. The editor's ImGui
integration is a small custom `ImContext`/`ImPlatform` pair (~3-5 methods
each), not upstream `imgui_impl_*`.

**Vendored deps that are Windows binaries with no source** — these still need
real upstream source or Linux SDKs, not a recompile: Optick, nativefiledialog,
FMOD. (RTTR was the fourth; see "RTTR is sorted" above.)

**Watch for other MSVC-isms.** `VColors.h` defined `GLOBALCONST` as
`extern const __declspec(selectany)`, which broke every TU that included the
pch; it is now `inline const`. Expect more of these as further files are
brought into the build.

**`std::cosf`, `std::sinf` and `std::fmodf` are not standard**, and this one is
a trap because it compiles locally. The C `cosf`/`sinf`/`fmodf` are only
guaranteed in the *global* namespace; whether `<cmath>` also puts them in `std`
is a libstdc++ version detail. The dev machine's does, the CI runner's does not,
so eight call sites in `RenderContext.cpp` and `Bullet.cpp` were red in CI and
green here. They use the unsuffixed names now - the `float` overload is the same
function. To sweep for more:

```bash
grep -rnoE "std::(abs|ceil|cos|exp|fabs|floor|fmod|log|pow|round|sin|sqrt|tan|trunc)f\b" Voxagine/Source Game/Source
```

It was the first thing turning `VOXAGINE_BUILD_ENGINE` on in CI found, which is
the argument for having done it.

## `Settings.vgs` was never read, and that is why presentation felt laggy

The most expensive kind of bug: three layers of silence stacked on one dropped
object file. Worth reading in full before adding a reflected type or debugging
a setting that "does nothing".

**`Core/Settings.cpp` was dropped from every link.** It is 51 lines - one
`RTTR_REGISTRATION` block and one out-of-line setter nothing calls. RTTR
registrations are static initializers, so a reflection-only translation unit
exports nothing anyone references, and a linker pulls an object out of a static
archive *only* when something references a symbol in it. `libvoxagine.a` handed
the linker `Settings.cpp.o`; the linker declined it. `nm` on either binary found
no `Settings::SetFullscreen`.

**So the `Settings` type had zero registered properties**, and nothing anywhere
said so:

- `FromJsonFile` returned `true` as soon as the file *opened* - it never
  checked `doc.Parse` for errors or whether anything was applied.
- `FromJson` logs only when `Data`/`Type` are missing. They were present.
- `rttr::type::get_by_name("Settings")` is *valid* for an unregistered type -
  RTTR names types it has merely seen - and `get_properties()` on it is
  **empty rather than invalid**. The apply loop iterated zero properties.

The file parsed, the type matched, nothing was applied, no error was logged, and
every value silently stayed at its compiled-in default. `FrameLimit`,
`EnableVSync`, `Fullscreen`, `FXAAEnabled`, `ShadowsEnabled`, `ResolutionScale`,
`LockedAspectRatio` and `InitialWindowSize` were **all inert for the entire
port**.

**That is the whole "presentation feels laggy" mystery.** `Settings.vgs` asks
for `FrameLimit: 0.0` and `EnableVSync: true`; the engine ran at the default
1/200 with `MAILBOX`. 200 fps into a 60 Hz display means the displayed frames
advance the world 15 ms, then 20, then 15 - 200/60 is not an integer - so motion
reads as skipping while every frame is delivered on time and every frame-time
log is clean. It was never findable from inside the frame loop, which is exactly
where phase 4 looked. Confirmed by the user on screen: FIFO at 60 fps, smooth.

Three fixes, all of which should stay:

- **`BitBuster` links `voxagine` with `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>`.**
  Fixed at the link rather than by giving `Settings.cpp` an anchor symbol,
  because any future reflection-only TU has the same shape. This immediately
  exposed a real duplicate that dead-stripping had been hiding: `imgui_stl.cpp`
  and `imgui_stdlib.cpp` are the same upstream file under two names and define
  the same symbols. Only `imgui_stdlib.cpp` builds now.
- **`FromJsonFile` reports parse errors and returns false**, so the caller's
  "rewrite with defaults" branch can actually fire.
- **`FromJson` errors when a type has no reflected properties.** An empty
  property list means a registration that never ran, and it is the cheapest
  possible detector for this whole class.

**`EnableVSync` now does something.** `VKSwapchain` takes FIFO when it is set
and mailbox where offered when it is not, and the editor has a **View → V-Sync**
toggle that rebuilds the swapchain live (the present mode is baked into
`VkSwapchainCreateInfoKHR`). Which of smoothness and latency matters more is a
judgement, which is why it is a setting.

**Generalise this.** A setting that appears to do nothing is not necessarily
unwired code - check that its reflection registration is actually in the binary
before reading the code that consumes it. `nm -C <binary> | grep <Type>::` is a
ten-second test and it was the whole answer here.

## The voxel buffer has a second, coarser copy now

`Docs/RENDERING_PLAN.md` phase 2 added `VoxelBrickGrid` — one count of occupied
voxels per 8³ block of the resident window, which the marcher walks so it can
skip empty space instead of stepping through it. Three things follow that are
easy to get wrong later:

- **Every write into the voxel buffer must maintain it.** `ModifyVoxel` and
  `ModifyVoxelFast` do it for point writes; `ChunkSystem::RenderChunk` does it
  in bulk via `BeginRegion`/`AddVoxel`/`EndRegion`. A new write path that
  writes `m_pVoxelData` directly will not, and the symptom is geometry quietly
  missing from the image — a brick counted at zero is never descended into.
  **View → Validate Occupancy Bricks** in the editor recomputes the whole grid
  *and the occupancy bitmap* from the voxel buffer and names the disagreements.
  It is the only correctness test here that does not depend on the camera:
  resident occupancy legitimately swings by a million voxels as the window
  slides, so comparing occupied-voxel totals between two runs proves nothing
  unless nobody touches the camera. It freezes the editor for as long as it
  takes to read 75 M voxels back over PCIe, which is expected, not a hang.
- **The counts live in ordinary CPU memory, not the mapping.** The mapping is
  uncached, so a read-modify-write performed on it directly costs an uncached
  read per voxel. The mapped buffer is a write-only mirror.
- **The mapper now prefers `DEVICE_LOCAL | HOST_VISIBLE` (ReBAR)**, falling
  back to plain host-visible. Big GPU win, but it makes every CPU *read* of the
  voxel buffer a PCIe read of VRAM: `VoxelBaker::Bake` went 0.09 → 0.22 ms on
  that change alone. Adding reads to the CPU voxel paths is more expensive than
  it used to be.
- **Nothing on a voxel write path may read the mapping back, and nothing has
  to.** `VoxelBrickGrid` carries a third representation for exactly this: one
  bit per voxel of the window (9.4 MiB a buffer), in ordinary cached memory,
  answering "was this occupied" — the only question a count update needs. That
  read used to come from the voxel word itself and cost a PCIe read of VRAM per
  baked voxel: **5.3 seconds** of a world load, 74 ms of a chunk load. Both
  `ModifyVoxel` and `ModifyVoxelFast` are now write-only with respect to the
  mapping, and `Docs/RENDERING_PLAN.md` phase 4b has the numbers.
  `SetVoxel` reads the old occupancy itself rather than taking it from the
  caller, so the bit and the count always move together or not at all.

**The bricks are now one level of a five-level pyramid** (`Docs/RENDERING_PLAN.md`
7.1b): 2³, 4³ (the bricks), 8³, 16³, 32³ over the same window. It exists
because a cone traced against the 4-voxel bricks alone cannot serve ambient
occlusion — that was established on screen, twice, and the plan has the two
failures.

**It has two GPU forms and they are not interchangeable.**

- The **brick level only** is counts in the storage buffer phase 2 added, at
  element zero. That is what the marcher reads: one exact count of one cell,
  addressed by integer index through `PosToBrickID`/`VoxelPyramid.hlsl`. The
  far field keeps its own `VoxelBrickGrid` and gets the same.
- **Every level** is a mip of `voxelPyramid`, an R8 3D texture holding the
  occupied *fraction* of a cell. That is what the AO cones read, because a cone
  wants its own width filtered and one `SampleLevel` is the whole operation.
  Route A put all five levels in the buffer and filtered by hand; that cost
  4.6x the rest of the cone and is why the texture exists.

**Only mip 0 is uploaded.** `FlushDirty` writes a host-visible density mirror
of the finest level and records a box per run of dirty bricks; the frame copies
those boxes into mip 0 and the rest of the chain is a chain of
`vkCmdBlitImage`. **Do not "fix" that into a per-level upload**: a Vulkan mip is
a floor-halving while a pyramid level is a ceil-div of the window, they agree
only where the window is a multiple of 32, and where they disagree a level
uploaded by index lands on the wrong texels with nothing to say so. A linear
blit resamples whatever extents the image has, and for an exact halving it *is*
the eight-child average.

- **Only the bricks are maintained per write. Everything else is deferred to
  `FlushDirty`**, called once a frame from `RenderContext::Present`, which
  rebuilds each dirty brick's fine cells from the occupancy bitmap and each
  coarse level as the sum of its children. Do not "fix" this back into the
  write path: maintaining all five incrementally was built and measured, and it
  took `CPU VoxelBaker::Occupy (added)` from 1.86 to 7.20 ms — a second added
  to a world load — because a 32³ cell is written by up to 32768 voxels of the
  same burst and each write redid the same cell. It is 1.85 ms deferred, with
  `FlushDirty` at 0.03 ms a frame and 19.4 ms once when a world finishes
  loading.
- **A read of any level but the bricks is only as current as the last flush.**
  Tests and audits call `FlushDirty` first; `Validate` does it itself.
- **`Validate` covers every level**, and the same editor menu item runs it:
  10.8 M cells against 75.5 M voxels. It proves the *counts* against the voxel
  buffer and nothing more.
- **`ValidateVoxelPyramid` is the other half and is the one route B needed.**
  **View → Validate Coverage Pyramid**, or `VOXAGINE_PYRAMID_AUDIT=<seconds>`:
  it reads the texture back and checks mip 0 against the mirror exactly, and
  each coarser mip against the average of its children. The failure it exists
  for is a dirty region that never reached the GPU — the counts stay right, the
  image stays plausible, and the lighting describes geometry that is no longer
  there. It found exactly that on its first run, so it is not hypothetical.
  Run it *during destruction*, for the same reason `VOXAGINE_SYNC_AUDIT` wants
  to be: the disagreement is produced by a write and does not exist at rest.
- **The density staging buffer is single-buffered against the frame**, like the
  particle mapper: the CPU rewrites it while the GPU may still be copying last
  frame's boxes. Accepted, because the only thing that can tear is one byte of
  one cell of an occlusion term and every byte is a valid density.
- **Cone AO against the pyramid is on** — `AO_CONE_ENABLED`, and the voxel pass
  is 2.44 → 3.47 ms at 4K for it. Sampling one mip per step with a *point* mip
  filter is deliberate: blending two levels is smoother and costs twice what
  route B bought.

**A binding trap this uncovered, worth knowing before adding any shader
resource.** In `VKPassBindings.cpp`, `E_STORAGE_BUFFER` is the one binding kind
that does not determine its own HLSL register class — `StructuredBuffer` is `t`,
`RWStructuredBuffer` is `u`, and Vulkan calls both a storage buffer.
`MakeBinding` guesses `t`. Anything read-write must go through
`MakeUnorderedStorageBuffer` or it silently lands on top of whichever texture
holds that register. `AddBuffers` had the fix inline; `AddMappers` did not,
because no mapper had ever been read-write without a colour format until the
brick grid. Validation catches it, which is why the renderer work runs with
validation on every phase.

## The bake was not slow, it was redundant

`Docs/RENDERING_PLAN.md` phase 4c. **`CPU VoxelBaker::Bake` went 756.7 → 0.03 ms in
Release, but the world load only got about 2.9x faster, and the difference
between those two numbers is the thing worth remembering.**

The bake was writing back what was already in the buffer: `OnComponentAdded` stamps
every renderer, `Chunk::UpdateRenderer` then requests an update for every one on
first load, and the clear-and-re-occupy that followed reproduced the same voxels
exactly — **218 of 218 byte-identical**, instrumented before anything changed.

`VoxRenderer::BakeData::StampKey` now records what the stamped voxels are a
function of — the stamp transform plus the frame, override colour and render
state, which is exactly what `ForEachStampedVoxel` reads — and
`RenderContext::GetVoxelGeneration` records whether the buffer still holds the
last stamp. Together they answer "would re-baking change anything?" in O(1).
**A forced update now re-examines every renderer rather than re-stamping it.**

**The timer covered two of the three stamping passes, not all three.** A world
load stamps every renderer once in `OnComponentAdded` — 587 renderers, 3.1 M
voxels, **399.5 ms** — and that call has never been inside `Bake`. So:

| | stamping at a world load | |
|---|---|---|
| before | `OnComponentAdded` 399.5 ms + `Bake` 756.7 ms | **~1157 ms** |
| after | `OnComponentAdded` 399.5 ms + `Bake` 0.03 ms | **~400 ms** |

Three passes became one: 2.9x, exactly what the structure predicts. The 30,000x
on the named counter is real but says only that *all* of what that counter
measured was the redundant part. `OnComponentAdded`'s stamp now reports as
**`CPU VoxelBaker::Occupy (added)`** so the two are visible side by side; it is
the largest remaining piece of the world-load stall, and the one place where
the unexplained per-voxel cost below still matters.

Four things worth carrying:

- **Quote the cost of the operation, not of the function you instrumented.**
  A timer around part of a pipeline goes to zero when you delete that part's
  work, whether or not the pipeline got faster.
- **`BakeData::Updated` is not the same question and must not be used for it.**
  It is set from the *transform*, and a transform reaches the stamp through a
  quantized rotation and a `floor`, so it can move without moving a voxel. All
  218 had it set while producing identical stamps.
- **Check whether work is redundant before making it cheaper.** Phase 4b halved
  the per-voxel cost of this same bake (5.3 s → 757 ms) and was right to; but
  two thirds of the remaining work did not need doing at all, and no amount of
  per-voxel tuning finds that.
- **Locality was measured here and is not what the bake pays for.** The write
  stream really is scattered — 6.94 M distinct occupancy-bitmap words per 10.4 M
  writes — and making it coherent (17x fewer cache-line touches) changed the
  time per voxel *not at all*. Attribution by disabling parts of the write path
  disagreed with an isolated benchmark of the same structures by 5-10x and was
  never reconciled. If per-voxel cost matters again, start from that gap, not
  from cache behaviour. The `.vox` sort it produced was kept for a different
  reason: it takes the far-field build from 243.0 to 213.9 ms.

**`VOXAGINE_PROFILE=1`** forces the frame profiler on regardless of build type
(`0` forces it off). It defaults off in Release, which is precisely where the
one-shot costs behave differently, so every number above needed it.

## Dynamic renderers are invisible to the physics grid, and static bakes eat them

Reported as: drag a wall across a character in the editor and the character
loses its middle; move the character and it comes back. Pre-existing, not
something the phase 4c bake work introduced — verified by where the arbitration
reads from. Fixed, and confirmed on screen in all cases including the reverse
(dragging the wall back off leaves no character-shaped hole in it).

`VoxelBaker::Occupy` touches the physics grid only inside `if (bIsStatic)`, and
only a static renderer writes `Active`/`UserPointer` there. **A dynamic
renderer's voxels therefore live in the render buffer and nowhere else.** So
when a static renderer is stamped over them, its `bForceVoxel` test — *is this
cell free of an owner and inactive?* — answers yes, because the character was
never in the grid to be seen. It overwrites those voxels **and records them in
its own `BakeData::Positions`**, so its next `Clear` erases them. Nothing
re-stamps the character, because nothing about the character changed.

A *static* character is immune, which is the tell: the physics grid is exactly
what makes two static renderers visible to each other.

**The overwrite is load-bearing, so the fix is to repair rather than prevent.**
If the wall declined to write there, it would grow a character-shaped hole the
moment the character walked away. `VoxelBaker::NotifyClearedRegion` instead
marks the dynamic renderers overlapping what a static `Clear` just erased, with
`Generation = 0` — which already means "the buffer no longer holds what you
stamped", and is what stops the phase 4c stamp-key check from skipping a
renderer whose stamp is unchanged but whose voxels are gone.

**Static renderers are deliberately not notified.** They cannot be damaged this
way, and notifying them would let two overlapping renderers mark each other
every frame forever. That asymmetry is the cascade guard — do not "fix" it by
making the pass symmetric.

**The mirror image of this was still open and is now fixed too** — see "Four
ways a voxel goes missing" below. The wall records the character's voxels as its
own, so when the *character* moves off, its `Clear` erased voxels the wall was
drawing. `Clear` now declines to erase a voxel whose grid cell is owned by
somebody else *and* active.

## Four ways a voxel goes missing, and none of them are shading

Reported as flickering, as models clearing each other, and as a character
leaving its colours behind in a destroyed area. Four separate defects, all of
the same shape: **the voxel exists in the buffer, or should, and something about
rasterization or bookkeeping loses it.** Worth reading together, because each
one was found only after the previous fix made it visible.

**1. The AABB proxy did not contain the stamp.** `RenderSystem::PostTick` built
the proxy from `VoxRenderer::GetBounds()` — the transform matrix applied to the
model's corners — while `VoxelBaker` places voxels with a *quantized* rotation
and two `floor`s. Those are different quantities and the proxy is the smaller
one wherever they disagree: measured over a play session, **8.5% of proxy
submissions were short of the voxels actually stamped, worst case 3.5 voxels**,
and the offenders were exactly the things that rotate — `Player`, `Bullet`. The
voxel pass rasterizes proxies and nothing else, so those voxels were drawn only
at angles where some other model's box happened to cover the pixel. The proxy is
now the union of the old box and `ComputeStampedGridBounds` (`VoxelStamp.h`),
which derives the box from the same stamp transform the bake walks.
`VOXAGINE_BOUNDS_AUDIT=1` reports the shortfall; it measures the *old* box, so
it keeps reporting 8.5% — that is the thing being covered, not a live defect.

**2. A window slide silently deletes every dynamic renderer's voxels.** This is
the big one and it is not visible from anywhere near the baker.
`ChunkSystem::RenderChunk` rewrites each resident chunk's slice of the voxel
mapper's **back** buffer in full, from the chunk's own CPU voxels — which hold
static geometry and nothing else — and then swaps. A dynamic renderer never
writes a chunk's voxels, only the mapping, so **the swap drops its stamp
entirely**. Two consequences, both of which were live:

- Its `BakeData::Positions` name addresses whose contents are gone, and `Clear`
  shifts them by the offset delta onto addresses that now hold the freshly
  rendered static world. Erasing those punches a renderer-shaped hole in
  whatever the window slid over. `Clear` now drops a dynamic renderer's
  positions instead of replaying them.
- A dynamic renderer that has not *moved* is skipped by the bake, so nothing
  re-stamps it and it stays invisible until something moves it. `Bake` now
  treats a changed `WorldOffset` on a non-static renderer as a reason to
  re-examine it.

A static renderer is the opposite case and keeps the shift: its colour is in the
chunk's voxels, so `RenderChunk` puts it back at the shifted address.

**3. `Clear` erased voxels another renderer had taken over.** The mirror of the
static-eats-dynamic defect above. The test is the owner slot, which is ordinary
cached memory — reading the voxel back out of the mapping would be a PCIe read
of VRAM per cleared voxel. **The cell has to be *active* as well as owned**, and
that is not a detail: destruction zeroes a voxel's colour and leaves the slot
behind, so a blasted area is full of owned-but-empty cells, and a particle claim
reserves a cell without ever putting a colour in the buffer. Treating either as
an owner strands the clearing renderer's colour there for good — which is
exactly "walk a character through a destroyed area and it leaves its colours in
it", a symptom introduced and then removed inside one session.

**4. Voxels written by something that is not a renderer had no proxy at all.**
A particle that bakes itself into the world on impact (`PhysicsSystem`,
`BakeOnImpact`) writes a voxel that no `VoxRenderer` owns, so nothing submitted
a box for it. That is the settled debris pile at the foot of a destroyed model,
and it flickered as the camera moved because it was drawn only when some
unrelated proxy happened to cover the pixel. `RenderSystem::AddLooseVoxel`
registers them, bucketed into 32³ cells and boxed tightly around what actually
landed, and `SubmitLooseVoxelProxies` submits one proxy per cell overlapping the
window. **Kept in level space, not window space**, so the registry survives the
window sliding and a chunk unloading and coming back.

**`VOXAGINE_COVERAGE_AUDIT=<seconds>` is the acceptance test and is kept.** It
counts occupied bricks of the window that no proxy contains. The number that
matters is *above the ground row*, which must be zero: the y=0 brick row is
deliberately uncovered because post processing composites the endless ground
analytically instead of marching it, so the total will read in the thousands and
mean nothing. Confirmed zero across a long session with heavy destruction, chunk
loads and unloads. It does show 2–4 uncovered bricks for a single sample
occasionally — a renderer stops submitting a proxy the moment it is disabled
while its voxels stay in the buffer until that frame's bake erases them. One
frame, harmless, unfixed.

**Two method notes worth keeping.** The step-count heatmap
(`MARCH_STEP_DEBUG`) ruled out the marcher in one screenshot — the flickering
piles came back *dark blue*, meaning the rays were hitting almost immediately,
which killed a plausible theory about the step budget before any code was
written for it. And a throwaway "grow every proxy by N voxels" knob separated
*coverage* from *shading* in one run: if an artefact disappears as the boxes
grow, the voxels were outside every proxy. Padding is not a fix — a box is also
where a ray starts — but as a bisection tool it cost two minutes.

**Cost.** The repair scan is the only part of this that is per-renderer per
clear, so it is reported separately as `CPU VoxelBaker::Bake (repair scan)`:
**0.005–0.032 ms against a bake of 0.19–0.88 ms**, in gameplay with destruction.
It compares against the grid-space box the bake recorded
(`BakeData::StampMin/Max`) rather than rebuilding one from the transform matrix,
which is what made it cheap enough to stop worrying about.

## The CPU voxel is 4 bytes, and its owner lives beside it

`Docs/RENDERING_PLAN.md` phase 4d. `struct Voxel` is a bare `uint32_t Color` with a
`static_assert` holding it there; occupancy is `(Color >> 24) != 0`, the same
alpha-is-a-`rendererState`-tag rule the shaders and `VoxelBrickGrid` already
use. Ownership — what `uintptr_t UserPointer` held — moved into a
`VoxelOwnerVolume` parallel to each chunk's voxels: **one `uint16_t` slot per
voxel**, a per-world table from slot to entity id, and a sparse map for particle
claims under the reserved slot `0xFFFF`. A resident chunk went **128 → 48 MiB**
and measured peak RSS **1.696 → 0.946 GB**.

- **Read or write an owner through `VoxelGrid::GetCell`, not a bare `Voxel*`.**
  A `VoxelCell` is the voxel, its owner volume and its index resolved by one
  pass of the chunk index arithmetic. `GetVoxel` still exists and is correct for
  colour-only callers; it just cannot answer an ownership question any more.
- **`GetChunk` takes an optional parallel `uint16_t*` of slots.** That is how a
  callback with no coordinates — `Entity::OnVoxelCollision`, which is where
  `Bullet` computes its combo streak — gets ownership at all. A slot is a stable
  identity because slots are never recycled, so comparing slots is the right way
  to ask "same model?" and resolving them to ids is not.
- **Why not a sparse map, which the ratio begs for.** 970 K owned voxels over
  ~117 distinct entities looks like a hash map from every angle *except the one
  that matters*: `VoxelBaker::Occupy` writes an owner for **every stamped
  voxel**, 3.1 M of them in one burst at a world load. At 50-100 ns an insert
  that is 150-300 ms added to the largest remaining load stall. Check what
  *writes* a field before choosing how to store it; the read pattern argued for
  the wrong answer here.
- **`Active` is gone and every write that set it beside a matching colour was a
  pure deletion.** Verified both ways: identical active (2,706,503) and owner
  (2,116,679) counts on the same scene before and after.
- **The chunk RLE changed format** — (colour, slot) at 7 bytes a run rather than
  (bool, colour, `uintptr_t`, `';'`, count) at 18, and runs are longer because
  they no longer break wherever a raw owner pointer changed. Encode 16 → 7 ms,
  decode 54 → 14 ms. It is an in-memory format only, nothing on disk. Particle
  claims are dropped on encode; the old format restored `Particle*` values into
  a pool that had recycled them long before.
- **`VOXAGINE_VOXEL_AUDIT=<seconds>` is the acceptance test and is kept.** It
  reports occupancy, the slot population and dead owners, and round-trips every
  loaded chunk through the codec in place — the only way to exercise the RLE
  without walking far enough to unload a chunk. Run it *during destruction* too:
  a static scene has no particle claims, and owner-set-but-inactive (debris in
  flight, which blocks a static re-bake over that voxel) only exists then.

**A Debug-only link error this produced, worth recognising.** The slot constants
were `static const uint16_t` with in-class initializers. `vector::assign` and
`unordered_map::emplace` take their arguments by reference, so they odr-use the
constant and need a definition. Release inlined every use and linked fine;
Debug did not. `static constexpr` is implicitly inline in C++17 and fixes it —
and this is the second time this tree has shipped a Release-only link
difference, so **build both** (see "Release is a real configuration").

## The proxy cubes are already the ray's starting point

Worth knowing before optimizing the marcher again, because it is not obvious
and it cost `Docs/RENDERING_PLAN.md` phase 3 its whole premise.

`VoxelRenderer.vs.hlsl` hands the pixel shader `WorldPosition` on the
rasterized AABB proxy cube, and perspective-correct interpolation makes that
the point where *this pixel's* camera ray enters *that* box. So every primary
ray already starts at a per-pixel, exact, free box entry - not at the camera.
The marcher's `IsInChunk` bounds test does span the whole window, which reads
like "it marches the entire world", but the *origin* is tight.

Phase 3 built a low-resolution depth prepass to supply exactly that starting
distance and measured no win, because there was nothing left to skip. **It has
been deleted** — phase 4 turned out to march the far field against its own
volume, so nothing used it.

It was re-measured before removal, uncapped and at resolutions from 682x384 to
4K, because the original numbers were taken at a capped 200 fps and at a single
resolution. **The saving was a flat ~0.6% of the voxel pass at every
resolution** — it never grew relative to the work it optimized, which is the
opposite of its premise. Worth generalising: *a percentage that stays constant
as you scale the input is the signature of an optimization that is not
addressing the thing that costs.* Sweeping the axis an optimization claims to
attack is cheap, decisive, and better done before building it than after.

The same measurement found where the voxel pass's time actually goes: **the
shadow ray is 53% of it**, it is near-field cost rather than long traversal
(capping its reach changes nothing), and no start-distance trick can reach it.
Fewer rays - half-resolution shadow terms - is the only remaining lever.

## The far field is a second, much coarser copy of the *whole* level

`Docs/RENDERING_PLAN.md` phase 4. The resident window is a 3x3 chunk view, so the
horizon was always its edge; `FarFieldVolume` is the rest of the level at 4x
downsample (1536x128x1536 -> 384x32x384, 18 MiB) with its own `VoxelBrickGrid`,
marched in **post processing** for pixels the voxel pass left empty. Four things
worth knowing before touching it:

- **It is built from entity JSON, not from voxels.** A `.wld` stores only
  `RootEntities` per chunk. A chunk's `m_VoxelData` is a *product* of loading -
  the ground plane plus whatever `VoxelBaker` stamps - and `EncodeVoxels` runs on
  *unload*, so a chunk never visited has neither decoded nor encoded voxels.
  `FarFieldBaker` deserializes each chunk's static entities into throwaway
  entities that never enter the world and stamps their `VoxRenderer`s.
- **Placement is shared code.** `VoxelStamp.h` holds what was the middle of
  `VoxelBaker::Occupy`; the window passes `VoxelGrid::GetWorldOffset()` as the
  grid origin and the far field passes zero. That parameter is the only
  difference between the two grids, and if they disagree the seam moves as the
  window slides.
- **It is marched in post processing because no proxy cube covers it.** The
  voxel pass only rasterizes AABB proxies, so a pixel showing only far-field
  geometry is never rasterized there at all.
- **Post processing composites a *one-submission-old* image.** `Present` copies
  the voxel target at the top of the frame, so anything in post processing that
  reconstructs a world-space ray must use `sceneInvMvp`/`sceneCamPosition`/
  `sceneCamOffset` rather than the live camera, or it slides against the image
  underneath it. Sky and ground had this error for the whole life of the port and
  hid it, because they read only the ray's Y.

**The endless ground plane is not an approximation of the chunk ground plane -
it *is* it**, sampled out of the window's y=0 layer and tiled with `fmod`. Baking
a second copy into the far field was tried and reverted: the same surface in two
places with two different lightings put a hard brightness edge along the window
boundary. Related, and both fixed here: it was lit by a hardcoded `x 0.6` rather
than the voxel pass's own lighting, and it solved for y = 0 when the ground
layer's *top* face is y = 1 (`R_GROUND_PLANE_HEIGHT`, paired with
`GROUND_PLANE_HEIGHT` in `Defines.hlsl`).

**Colour the suspects; do not reason about a one-pixel artefact.** Four separate
defects lived on the window's boundary and became visible only once the far field
put lit ground on the far side of it. Three consecutive hypotheses about them
were wrong. What worked, in two build cycles, was diagnostic builds: one
colouring each pixel by *which code path drew it*, then one colouring hits by
*surface normal*. Both answered in a single screenshot. The four:
`IsVoxel` had no upper bound (`PosToVoxelID(x == worldSize.x)` is `(x=0, y+1)`,
so edge voxels were shaded from the opposite edge); the ground proxy box spanned
y `0..10` and so started inside the ground layer; `MarchBricks` gave a march
beginning inside a voxel a normal from the DDA's *next* crossing, which for an
origin on a face is that face; and FXAA blended every silhouette toward the
`float4(0,0,0,0)` a miss writes.

## The UI target's alpha is coverage, and two things were breaking it

The splash screen's background split into two differently coloured halves at the
exact vertical midpoint of the 16:9 target while its logos faded. It was neither
the opacity fade nor a UV problem, and it took **two** fixes - the first alone
looked like a fix at a still moment and did nothing during the fade, which is
the only time the artefact is visible.

**What the split actually was.** `PostProcessing.ps.hlsl` reads the UI target's
alpha as coverage: `uiColor.a == 1.0` takes the UI straight through, anything
less composites it against `GetBackground()`. So the two halves were **sky above
the horizon and the endless ground plane below it**, seen *through* a logo whose
alpha had stopped saying "fully covered". It sits on the vertical midpoint
because the splash camera's pitch is zero, which puts the horizon at screen
centre. Nothing was ever split - a flat background was being composited against a
background that is not flat.

1. **Sprites were submitted front to back.** `RenderSystem::PostFixedTick`
   sorted layers ascending, but `UIRenderer.vs.hlsl` maps a *higher* render
   layer to a *greater* depth and the UI pass depth-tests `LESS_OR_EQUAL` with
   depth writes on. The logos (layer -1) drew first and depth-rejected the
   opaque background (layer 8) everywhere they covered. Translucent geometry has
   to be drawn back to front regardless; the map is `std::greater` now.
2. **`VKRenderPass` overwrote destination alpha instead of accumulating it.**
   `srcAlphaBlendFactor = ONE` with `dstAlphaBlendFactor = ZERO` is literally
   `dst.a := src.a`, so a logo fading through alpha 0.5 *replaced* the target's
   alpha with 0.5 even over an already-opaque background. Now
   `ONE / ONE_MINUS_SRC_ALPHA`. Only the UI pass enables blending, so this
   changes nothing else - but it also means overlapping translucent HUD sprites
   composite by coverage now rather than the last one drawn winning.

**The lesson is the one phase 4 already learned, plus a second half.** The path
shader answered "which branch drew this" in one frame - it was sky over ground,
not the fade, and the `sceneFader` readout strip proved the fade was sitting at
1.0. But a single screenshot only samples one instant, and this artefact only
exists *during* a fade. **Capture the moment the artefact is in, not a
convenient one**, or a partial fix reads as a complete one.

Cheap and worth rebuilding if a compositing bug like this returns: a
`POSTFX_PATH_DEBUG` block in `PostProcessing.ps.hlsl` colouring each pixel by
which branch produced it (UI early-out / sky / ground / far field / FXAA /
silhouette), a greyscale `sceneFader` strip down one edge, and white cross-hairs
at `viewport.xy * 0.5` to tell "the midpoint of the target" apart from "the
midpoint of the window".

## The "freeze" after gameplay is a GPU timeout, and the marcher has no budget

Reported as the game freezing after some gameplay, renderer-only, with the log
still running. It is none of those things in the way it looks.

**What the log says.** `[stall] GPU has not completed for 600 frames: direct
10549/10551, vdirect 5294/5295` — `RenderContext` submitted and the fences never
signalled, so it returns early every frame. That is the `[fps] 0 (frame ms: p99
0.07, over 4096 samples)` that follows: the main loop spinning on the early
return, which is why the process looks alive and the window looks dead. The
diagnostic is not new; it came in with the port commit precisely for this.

**What the kernel says, which is the answer.**
`NVRM: Xid (PCI:0000:01:00): 109 ... CTX SWITCH TIMEOUT`. The GPU could not
preempt a draw that was still running. Not a device fault, not a validation
error, not a shader infinite loop — **a frame that takes seconds**. Read it with
`journalctl -k --since today | grep Xid`; `dmesg` is restricted here.

**It is pre-existing and it is not phase 4d.** Nine Xid 109 events from
`BitBuster` on one day, seven of them hours before the phase 4d branch existed.
Reproduced on the pre-phase-4d binary. Check the kernel log before attributing a
"freeze" to whatever landed most recently.

**Why a frame can take seconds.** `VoxelRenderer.ps.hlsl` budgets the walk by
the window diagonal — `maxBrickSteps ≈ 137` on a 768³ window — and fires a
shadow ray with *the same* budget from every lit hit. `MarchBricks` runs its
24-step inner voxel DDA in every occupied brick it enters. So the worst case is
about 137 × 24 × 2 ≈ 6,600 voxel steps per pixel, and at 2732×2048 there is
**no per-frame ceiling of any kind**. Normal is ~1.7 ms and the last completed
frames before the hang measured 2.5 ms, so the pathological frame is a thousand
times the typical one rather than a gradual climb — something specific about the
camera or the destroyed geometry is the trigger, and that is not yet identified.

The 700-step cap `Docs/RENDERING_PLAN.md` phase 2 removed is exactly what used to
bound this. Removing it was right for image quality — it cut every long
sightline — but nothing replaced it as a *safety* bound, and a soft budget
(degrade the shadow ray, or cap total steps and accept a wrong pixel) is
cheaper than a three-second frame that the driver kills.

**That budget now exists, and measuring it moved the diagnosis.**
`MARCH_STEP_BUDGET` (1024, `Defines.hlsl`) caps the DDA crossings one pixel may
make across *all* its rays — primary and shadow share one counter, since what
needs bounding is the pixel's total work. The far field carries the same budget
under its own counter. Running out reports a miss, exactly as running out of
brick steps already did. Swept against the voxel pass's own GPU time, the pass
stops responding to the budget between 64 and 128, so 1024 clips nothing at the
usual vantage and costs nothing (2.58 ms with or without).

**It is a ceiling, not the explanation.** The same sweep prices the marginal
step at ~0.015 ms per step of the average, which makes a whole screen at the old
unbounded worst case worth about **100 ms** — and the hang is a frame of
seconds. Per-pixel step count is short by ~25x, so the missing factor multiplies
it rather than living inside it: fullscreen is ~4x the pixels this was measured
at, and the voxel pass runs a **full march per AABB proxy covering a fragment**,
with early-z the only thing keeping overdraw small and nothing bounding it in
principle. 4x resolution and ~6x overdraw is the gap. **Check the proxy count
first** — the `[stall]` line already prints `voxel instances` and `aabbs`, and
the log quoted above predates those fields.

**Reproduce with `MARCH_STEP_DEBUG` on** (commented out in `SDFMarcher.hlsl`):
it shades both voxel pixel shaders by step count, white where a pixel ran out,
including on the miss path — a long ray that finds nothing is the expensive kind
and is invisible in the normal image. White pixels name the geometry; *no* white
pixels during a slow frame rules the marcher out and points at overdraw.
`Docs/RENDERING_PLAN.md` phase 6.0 has the sweep table.

**A hypothesis that was wrong, recorded so it is not retried.** `Monster::
RangeAttack` really does produce a NaN rotation — it tested `direction !=
Vector3(0)` *before* flattening y, so a monster vertically aligned with its
target normalized a zero vector — and `RenderSystem::PostTick` really does
submit the proxy AABB with no finiteness check. Both are real. Neither is this:
a build with both fixed still hung, and the non-finite proxy warning never
fired.

## One voxel write, one place: `VoxelEditBatch`

`Docs/DESTRUCTION_PLAN.md` phase 1. A voxel exists in six places — the CPU colour in
the chunk, the word in the mapped GPU buffer, one bit of the occupancy bitmap,
one unit of a brick count, the owner slot, and (for a voxel no renderer owns)
the loose-voxel registry. Nothing in the type system said a write had to touch
all six, and most of the destruction defect ledger was call sites that touched
some. Three sites made the paired `ModifyVoxel` calls and **none of them
cleared the owner slot**, so three of every four destroyed voxels went on naming
a model that had nothing there any more.

`Core/Voxels/VoxelEditBatch.{h,cpp}` is now the only way destruction, island
conversion and bake-on-impact write. Build one from
`RenderSystem::MakeEditTarget()`.

- **It applies immediately.** It is not a deferred transaction despite the name
  — the integrity checker reads the grid straight after a burst. What it
  accumulates is the **dirty-brick set**, which is what phase 2's seeding and
  phase 4's connectivity want.
- **A clear clears the owner**, and that is why both particle-spawn sites now
  write their claim *after* the clear rather than before. The old order worked
  only because the old clear left the owner alone.
- **`VoxelGrid::ModifyVoxel` is gone.** It wrote a colour and nothing else, with
  no bounds check and no residency check. `GetVoxel`/`GetCell` remain for reads.
- **Non-finite positions are rejected centrally, and that is not tidiness**: a
  NaN passes every comparison in a rejection test, so a check written as "reject
  if outside" lets it through to an `INT32_MIN` index. That has cost this tree a
  two-billion-element overwrite once.

**Non-residency is now an ordinary state.** `ChunkSystem` calls
`VoxelGrid::DetachChunkStorage` on the main thread *before* enqueuing the unload
job, because `Chunk::EncodeVoxels` frees the chunk's voxel vector and owner
volume on a worker while the grid still pointed at both. Every accessor —
`ResolveIndex`, `GetCell`, `GetVoxel`, `VoxelCell::IsActive` — checks for it.
Detaching is by *storage*, not by location: an unloading chunk's slot may
already have been re-pointed at a chunk that moved into it.

## The integrity checker used to throw its answer away

`Docs/DESTRUCTION_PLAN.md` phase 4, and the fix is one observation: the flood fill
*discarded* everything it had collected the moment it touched the ground. So the
next seed standing on the same building flooded the whole building again — and
one explosion produces thousands of seeds on the same few structures. That is
the shape of #27, where integrity cost grew with how much had ever been
destroyed.

It records both answers now, and a walk that meets an already-grounded voxel is
itself grounded and stops there. Over the gauntlet: total integrity cost 47 ms
(phase 0) → 36 ms (phase 2's better seeding) → **17 ms**, with the same voxels
converted and the same final state.

**Invalidation is polled, not pushed.** `VoxelEditBatch` bumps
`VoxelGrid::GetWriteGeneration()` on every write and the checker compares it on
entry. The first version had callers invalidate, and the gauntlet — which writes
through the same batch but is not `PhysicsSystem` — kept a stale memo and found
20 islands where it should have found 100. A new write path cannot forget.

**`VOXAGINE_INTEGRITY_AUDIT=<seconds>` is the oracle and it stays.**
`IntegrityChecker::ClassifyExhaustive` is the pre-phase-4 walk kept verbatim —
no memo, no budget, no shared state, because an oracle that reuses the machinery
it checks proves nothing. Unlike the sync audit it walks cached CPU voxels, so
it costs seconds (about 19 on a 2.1 M-voxel window) rather than twenty of them.

**Its absolute number is not the finding; the delta is.** A pristine
`Fishing_Village_Beat1` reports **14,532 voxels in 211 ungrounded components**
before anything has been destroyed — decorative geometry, props resting on other
props, models whose only contact with the ground is through another model that
26-connectivity does not join. Nothing ever seeds those, so they simply stand,
which is what the level intends. The audit takes its first run as a baseline and
reports against it; a delta that grows with destruction and does not come back
down is the checker failing to find an island.

**What was *not* built: the per-brick component graph.** It was designed far
enough to find two problems — the component arena needs re-packing on every
dirty brick, and a brick with too many components needs a merge fallback that
makes the graph over-connected, which means islands that should fall do not.
That failure reads as level design rather than as a bug. The memo gets the same
asymptotic property with an exact answer.

## Particles own nothing, and that deleted a whole subsystem

`Docs/DESTRUCTION_PLAN.md` phase 3. A debris particle used to *claim* the voxel it
occupied — the reserved owner slot `0xFFFF` plus a sparse map from voxel index
to `Particle*`. It existed to answer "is this cell still mine" in flight and to
reserve a landing cell, and it failed at both: claims were dropped wholesale on
chunk unload, silently lost to takeover with the bake skipped, corrupted by
window slides, never persisted by design, and the one write meant to transfer
ownership to baked debris had never worked — so baked debris was already unowned
and the loose-voxel registry already carried it.

It is gone. `0xFFFF` is `k_uiReservedSlot`, **never written and never handed
out**: chunk data encoded before the change still carries it, slots are never
recycled, and the codec normalises it to "no owner" on encode. What was lost is
exactly one thing: a `VoxelBaker` stamp can now place colour in a cell a
particle is flying through — one overlapping cube for a frame or two.

**`ParticleCore` (`Core/Particles/`) replaced `ParticleLinkedList`**: structure
of arrays, swap-compacted, with a sparse-set handle (slot + generation) for
anything that needs to refer to a particle across frames. Five ledger defects
died with the old shape rather than being fixed — the old `Particle` was a
*union* of live state and free-list link, which is why a retired one was drawn
one extra frame from its own link reinterpreted as a float3.

Three things worth not undoing:

- **Grid position is derived, never stored.** A window slide changes what a grid
  coordinate means; caching one per particle is why any slide shorter than the
  100-unit teleport clamp left every particle in flight touching wrong cells.
  Position is level space.
- **Debris and the component emitters have separate budgets and write disjoint
  ranges of the GPU buffer** — debris from index 0, emitters above it. One
  shared running cap consumed in tick order is what let a busy emitter leave a
  destruction burst unsimulated *and* undrawn.
- **`ParticleLanding::Resolve` answers with one position**, which the batch turns
  into colour, occupancy, brick count, owner and loose-voxel registration. The
  old bake computed two and used them inconsistently.

**Known, deliberately left: the particle mapper is still single-buffered.** The
CPU writes records every fixed tick with no fence against the frame being read.
The fix is a back buffer plus a swap, but the swap must happen only on frames
that ran a fixed tick — otherwise a frame presents the previous tick's particles
— and that is a presentation change that needs somebody watching the screen.

**The loose-voxel registry records which voxels debris wrote**, not just a box
around them. Retirement used to ask the brick grid whether anything at all was
still occupied nearby, which static geometry satisfies, so a cell over a wall
never retired and its box re-tightened onto the wall.

## The explosion is an algorithm, not a method

`Docs/DESTRUCTION_PLAN.md` phase 2. `SphericalDestruction::Apply`
(`Core/Voxels/SphericalDestruction.h`) is the loop; everything gameplay-shaped
is a callback, so it needs no `World`, no entities and no particle pool.
`PhysicsSystem::ApplySphericalDestruction` supplies the destructibility test and
the debris spawner and does nothing else. That is what lets the gauntlet and the
unit tests drive the shipping algorithm rather than a copy.

Four things it fixed that are worth not reintroducing:

- **The loop carried a running position** incremented at the top and read as
  `volumePos.x - 1`, with row-wrap corrections that did not agree with the flat
  index into the fetched box — so on a wrap the cleared voxel, the sphere test
  and the voxel being read were three different cells. Coordinates are computed
  per cell now and distances are compared squared.
- **The colour came back out of the mapping**, one uncached PCIe read of VRAM
  per destroyed voxel, when the CPU voxel was already resolved. Same defect was
  in `VoxFrameEmitter`, fixed with it.
- **The radius was unvalidated**: `(uint32_t)fRadius * 2` on a negative or NaN
  radius is undefined and `diameter³` overflows `uint32_t` above 1625. Capped
  at 64 and logged once.
- **A voxel exactly at the centre normalised a zero vector**, which is NaN — and
  a NaN velocity becomes a NaN position, a non-finite proxy AABB and a frame the
  marcher never finishes.

**Integrity seeds are collected after the clear, not during it.** The old rule
pushed nine hashes per destroyed voxel; `CollectSeeds` seeds occupied voxels
with an empty face neighbour around the hole. That is a surface rather than a
volume, and a strict superset of the old set — an old seed was an occupied voxel
sitting on a destroyed one. Total integrity cost fell 22% over the gauntlet with
the same 100 islands found.

## The destruction harness runs without a GPU, and that is the point

`Docs/DESTRUCTION_PLAN.md` phase 0. Everything the destruction path touches is
ordinary memory except the mapped voxel buffer, and that is a `uint32_t*` the
engine only ever *writes* (rule 1 — reads come from the brick grid's CPU-side
bitmap). So the whole write path runs in a unit test at full fidelity by handing
it a plain array. `VoxelWorldHarness` does exactly that: a real `VoxelGrid` with
real chunk storage, a real `VoxelBrickGrid`, and a `std::vector<uint32_t>` where
the mapping would be.

**One test system: `voxagine_tests`, three modes.** `Tests/README.md` is the
reference. It replaced a gtest suite, a `voxagine_gauntlet` and a
`voxagine_selftest` — three binaries, three output formats, and *two copies of
the destruction tick loop* that had already drifted apart on the destructibility
predicate and the debris density. There is one driver now,
`Tests/Harness/DestructionRun.h`, and scenarios and benchmarks both run it.
Directories name the system under test (`Destruction/`, `Integrity/`,
`Particles/`, `VoxelEditing/`, `VoxelStorage/`, `Foundation/`), not the engine
directory a header happens to live in.

- **`voxagine_tests checks`** — assertions about one unit at a time. 70 of them,
  0.1 s. The macros in `Tests/Framework/Check.h` replace gtest's; `CHECK_`
  records and carries on, `REQUIRE_` abandons the case, both take a trailing
  `<< message`.
- **`voxagine_tests scenarios`** is where a destruction change should be proved:
  31 scenarios × 5 invariants, each run twice for determinism, **3.3 s in
  Release and 36 s in Debug**. Adding a case is adding one file; adding an
  invariant applies it to every existing case.

  It earns its keep: it found three separate causes of floating debris in its
  first run, rejected an attempted fix that would have collapsed level
  decoration, and caught a phase 3 regression where debris began landing on
  characters. **A defect that is not expressible cannot be caught** — two of the
  four play-session defects needed `VoxelWorldHarness::SetDynamic` to exist
  before any test could fail on them.
- **`voxagine_tests perf`** is the regression gate, and the thing worth
  internalising is *why it can be one at all*. It reports two kinds of metric.
  **Work** — voxels destroyed, cells the flood fill visited, seeds filtered,
  islands emitted — is exact, machine-independent and **identical in Debug and
  Release** (verified; CI checks it on both). Any increase over
  `Tests/Baselines/perf.txt` fails the run, with no flakiness whatsoever.
  **Time** is not, so timings are printed and never enforced unless `--strict`.
  The shared baseline holds *only* work metrics for exactly that reason.

  That split is the whole design. A rewrite that walks twice as many cells is
  invisible in wall time on this machine and fatal on a slower one; the visit
  count catches it exactly. For a timing comparison, record your own on both
  sides of the change (`--record before.txt`, then `--baseline before.txt`) on a
  quiet machine. Re-record the shared one with `--record-work` when a change
  legitimately moves the work, and say why in the commit.
- **Scripting the running game cannot be made deterministic**, which is why the
  harness exists rather than a `VOXAGINE_GAUNTLET` env var. The level is full of
  entities with their own randomness, the camera slides the window, chunk
  streaming runs on job threads, and the frame rate decides how many fixed ticks
  a second contains. The in-game audits are the integration half; this is the
  algorithmic half. Both are needed and neither replaces the other.
- **The particle sweep is code, not data.** `Particles/SimulationSweep` injects
  5 k / 50 k / 150 k debris into a standing level and reports **16.4 / 16.6 /
  16.2 ns per particle per tick** — flat over a 30× range, so 2.4 ms at the full
  150 k cap. That measurement closed `Docs/DESTRUCTION_PLAN.md`'s phase 6 gate. It
  used to be three `.gauntlet` script files; a sweep whose points live in a data
  file is a sweep nobody re-runs.
- **`VOXAGINE_SYNC_AUDIT=<seconds>`** is the in-game half: it compares the CPU
  voxel against the mapped word and re-derives the brick counts and occupancy
  bits, periodically, because the disagreements it looks for are produced by a
  *write* and so do not exist at rest. Also on the editor's **View → Validate
  Voxel Representations**. It reads the window back over PCIe, so it stalls the
  frame — expected, not a hang.
- **Particle velocities come from a seeded `DeterministicRandom`**, not
  `glm::linearRand`. The latter draws on a process-global engine that every
  other caller perturbs, so a replay diverged on the first unrelated call
  anywhere in the process.

## The floating bamboo: what it is, what it is not, and where it stopped

**Reported:** in `Fishing_Village_Beat1` (the editor's startup world) `Bamboo7`
and `Bamboo8` "consistently refuse to fall down on penetration" — you shoot
them, the middle goes, and the rest hangs in the air. **Not fixed. Read this
before picking it up again**, because two confident diagnoses died here and
both are the kind that look right until measured.

**The mechanism, measured.** `Obstacle_Pillar_Small_Tall_Bamboo_4.vox` is not a
pole. It is **three stalks one voxel thick**, up to 9 voxels apart, 26-connected
to each other only at the model's base layer and at two rejoin layers. A pole
falls only when a burst removes *every* voxel of its grounded layer
(`IntegrityChecker::IsGrounded` is `y == 1`). Miss one stalk and the whole pole
hangs off that single voxel — geometrically grounded, visually floating. That is
the symptom, exactly.

**Two hypotheses that were wrong. Do not re-derive them.**

- **The river.** All three destructible bamboos' *bounding boxes* intersect
  `RiverPiece3`, a 64x13x64 indestructible slab, which makes a beautiful theory:
  a prop embedded in indestructible geometry can never become an island. Stamped
  for real, the river touches **0** of the bamboo's voxels. Box overlap on a
  64-wide terrain model proves nothing.
- **The sinking.** `ComputeVoxelStampTransform` places a model by its *centre*
  (`offset = -fittedSize * 0.5`), so `Bamboo7`/`Bamboo8` at world y = 5 with a
  27-tall model stamp from grid y = **-9** and lose **33 of 96 voxels** below the
  world, while `Bamboo9` at y = 15 stamps from y = 1 and is whole. Compelling,
  and it is a real defect (below) — but swept over 81 aim points it moves the
  outcome by **3**. It is not the cause.

**What is uncommitted in the tree** (nothing of this is on master):

- **`VoxelStamp.h` is parameterised.** `ComputeVoxelStampTransform` and
  `ForEachStampedVoxel` no longer need a `VoxRenderer*`; they take a
  `VoxelStampModel`/`VoxelStampPose` and a `VoxelStampVoxels`, with the renderer
  versions as thin adapters (`DescribeVoxelStamp`). Same move as pulling
  `SphericalDestruction::Apply` out of `PhysicsSystem`. **This is the piece
  worth keeping regardless of the rest** — it is what lets a headless test place
  a real model at a real world transform, which the suite could not do at all
  before, and it is why every scenario until now was a box on flat ground.
- **`Tests/Harness/VoxModelFile.{h,cpp}`** reads a `.vox` without the engine's
  resource stack, reproducing `VoxModel::Read`'s axis swap, x/z flip and fit.
  **`VoxelWorldHarness::StampModel`** places it and returns how many voxels fell
  outside the world. CMake passes the content root as
  `VOXAGINE_TEST_CONTENT_DIR`, so this works in CI.
- **`Tests/Destruction/Scenarios/SunkenPole.cpp`** — the real bamboo at the
  level's own transforms.
- **A footing rule in `IntegrityChecker`**, described below.

**The footing rule, and why it is half-finished.** Touching the ground and being
supported by it are different questions. `HasFooting` says a ground contact
counts as support only if it has a solid neighbour in its own layer, or carries
a column no taller than `k_uiMaxPinnedColumn` above it. **All 31 scenarios pass
with it**, which is the whole point of having them — the first two attempts were
killed by the suite in seconds:

- *"A pin grounds nothing"* eats settled debris. A lone baked voxel at y = 1 is
  unsupported, converts back into a particle, lands in the same place and
  converts again, forever: ~100 new ungrounded components per scenario.
- *"A pin grounds anything up to one voxel"* still churned on 2-voxel stacks.

Hence a **load limit** rather than a yes/no, which terminates.

**Where it stopped, and the honest reason.** The metric is not trustworthy. The
aim sweep in `SunkenPole.cpp` reads 32 of 81 whether `k_uiMinFootingNeighbours`
is 1, 2 or 3 — and a number that does not move when you triple the parameter is
not measuring the thing. It is almost certainly dominated by aim points that
miss the model entirely, so it reports "did the shot connect", not "did the pole
fall". **Rebuild it to fire only at points that intersect the model before
trusting any threshold.** Then re-sweep, then run `voxagine_tests perf` — the
footing test adds up to eight grid lookups per ground contact and
`checker-visits` prices it exactly.

**Two existing checks fail under the rule** and would need rewriting to the new
definition rather than patching: `IntegrityChecker/CuttingASupportTurnsAStructureIntoAnIsland`
and `IntegrityChecker/AWriteThroughTheBatchInvalidatesTheMemo`. Both encode
"touching the ground is sufficient".

**If thresholds cannot separate "pole held by two voxels" from "legitimate
narrow base", this needs a real load model** — supported mass against supporting
cross-section — and that is a bigger piece of work than a threshold. Say so
rather than tuning constants until the suite goes quiet.

## Placement silently drops everything below the world

Found while chasing the bamboo, unrelated to it, and still open.

`ComputeVoxelStampTransform` centres a model on its transform, so a renderer
whose y is less than half its model's height has its base clipped off — and
nothing anywhere reports it. **853 of 8039 renderers across 21 levels are
clipped; 141 of them are destructible.** Worst cases are entities at negative y
losing 83 of 90 layers.

Most of the 712 non-destructible ones are deliberate — `Obstacle_Wall_Long_Long_Foundation`,
`Building_Floor1`, `RCLR_Small_Straight_Foundation`; burying a foundation is how
you hide a seam, and raising those would break the levels. **Do not mass-edit
this.** The agreed scope was "correct only what is proven broken", and nothing
has been proven broken yet.

What is worth building regardless: `VoxelBaker::Occupy` should count the voxels
it drops for being outside the world and warn once per renderer, naming the
entity. `StampedGridFloor` (in `VoxelStamp.h`, uncommitted) already computes the
lowest layer a stamp writes into, which is all such a check needs. An editor
validation pass over every renderer would turn this from a play-session
discovery into an authoring-time one.

## Known defects

- **`VoxelGrid` data race — *the one that is left*.** The `IntegrityJob`
  version of this is gone: the job was deleted in #19 and the traversal runs on
  the main thread now. What survives is chunk streaming.
  `Chunk::EncodeVoxels` frees a chunk's voxel vector and owner volume on a job
  thread while `VoxelGrid::m_ChunkVolumes`/`m_ChunkOwners` still point at them,
  so a main-thread `GetCell` can read freed storage. Tracked as
  `Docs/DESTRUCTION_PLAN.md` ledger P7 and closed by its phase 1. Nothing else
  touches the voxel array off the main thread.
- **Chunk loading stalls the frame.** *Mostly fixed.* `LoadEntities` is gone:
  chunk streaming phase 3 split it into budgeted staging and budgeted
  admission, and the 44 ms per chunk it cost is now 7.4 ms of staging spread
  across frames. What remains true is the sentence after it - every texture a
  load pulls in costs a full `Execute`/`WaitForGPU` GPU round trip. Batching them into one submit per
  frame is the obvious fix and **was tried and reverted**: the `Texture` engine
  is shared - `VKImContext::BuildFontTexture` and anything else calling
  `LoadTexture` drive the same one - so another user's `Execute` closes the
  command buffer mid-batch and the next recorded barrier hits
  "vkBeginCommandBuffer was not called prior to this command". Batching needs
  an upload engine that is not shared, or an explicit begin/flush protocol on
  the shared one. Don't retry it without that.
- **Raw pointers to resources outlive the resources.** Switching worlds
  crashed because `NullAudioContext` keeps the `SoundReference*` it is playing
  and nothing told it when the old world's sounds were freed. Fixed for the
  BGM by having a reference notify its context on destruction, but the same
  shape exists elsewhere: `AudioSystem::m_AudioSources`, and every manager that
  hands out raw pointers into a per-world resource set. Worth auditing as a
  class of bug rather than one at a time. **A second instance turned up in the
  editor** and is fixed: `EditorReferenceManager::ImportFiles` called `delete`
  on an object `ReferenceManager` still owned. `ReferenceManager` is the owner
  of everything `ResourceManager::Load*` hands back - callers get a borrowed
  pointer and release it with `Release()`, never `delete`. Nothing in the type
  system says so, which is what makes this a class rather than an incident.
- **A renderer disabled this frame keeps its voxels but stops submitting a
  proxy**, so for one frame they are in the buffer and outside every box.
  `VOXAGINE_COVERAGE_AUDIT` catches it as 2-4 bricks in a single sample. The
  bake erases them later in the same frame; nobody has reported seeing it.
- **Something produces NaN transforms.** `VoxelBaker` reports one, once, naming
  the entity. `SafeNormalize` fixed the known sources (a stopped monster's zero
  velocity, a zero impact normal), but the warning can still fire, so at least
  one more remains. GLM does not zero-initialise and `GLM_FORCE_CTOR_INIT` is
  not set - `Quaternion rotation;` in `Monster::RangeAttack` is one uninitialised
  value still in the tree.
- **`VKTextureManager` uploads one texture per submit,** each followed by a
  `WaitForGPU`, mirroring what the DX12 path did. It works, but a level's worth
  of sprites is a level's worth of round trips; batching them onto one submit
  is the obvious improvement.
- No frame graph: pass order is hardcoded by name lookup in
  `RenderContext.cpp`, and barriers come from fixed per-pass rules rather than
  a resource dependency graph. The Vulkan rewrite is the moment to fix this.
- `PhysicsSystem::AccumulateManifolds` is O(n²) over all colliders with no
  spatial partitioning, despite the chunked `VoxelGrid` being right there.
- `Editor.cpp` is a ~1,700-line god object (menus, input, save/load, window
  management, play state). The subsystems beneath it — `PropertyRenderer`
  (genuinely RTTR-driven) and `UndoRedo` (real command pattern, delta-based) —
  are well-factored and worth preserving as-is.
- ~~**`Chunk` never frees its encoded reserve.**~~ **Stale — this was checked
  and it is not true.** `EncodeVoxels` ends with `m_pEncodedVoxelData.
  shrink_to_fit()` (`Chunk.cpp:205`) and `DecodeVoxels` ends with
  `clear()` + `shrink_to_fit()` (`:251-252`), so the 10 MB `reserve` is a
  transient encode-time spike, not a retained cost: an unloaded chunk keeps its
  *actual* compressed blob and a loaded one keeps none. `m_VoxelData` and the
  owner slots are shrunk on unload as well. What remains true is that a
  resident chunk is 48 MiB and nothing changed how many are resident. Corrected
  while sizing the mobile memory budget — `Docs/MOBILE_PORT_LOG.md` phase 7.
- **The renderer's scale-up concessions are catalogued in
  `Docs/RENDERING_PLAN.md`.** AO, shadow fading and the sky came back in phase 1;
  the 700-step march cap and the shadow ray's double stride are gone as of
  phase 2. What is left is the resident volume being only a 3×3 chunk window
  of the level (phase 4). Don't fix these piecemeal; the plan orders them.

**Fixed by the port:** `DXCommandEngine::Reset()` used to call `WaitForGPU()`
every frame, serializing CPU and GPU. `VKSwapchain` waits per frame-slot fence
instead, so double buffering actually works. Don't reintroduce a global wait in
`VKRenderContext`.

**Found by the port, latent all along:** `glm::normalize` of a zero-length
vector is NaN, and `Monster::FixedTick` normalised its velocity every tick - so
a monster that stopped moving poisoned its own transform and stopped rendering.
`VoxelBaker`'s bounds check was a rejection test, which a NaN passes because
every comparison against it is false; the resulting `INT32_MIN` index wrote two
billion elements past the voxel buffer. Both are now guarded, and the queue is
mutex-protected because texture uploads submit from job threads while the main
thread submits the frame - the validation layers catch that as a threading
error.

## Conventions worth keeping

**Three environment variables added while chasing a play session, all cheap and
all kept.** `VOXAGINE_GAMEPLAY_DEBUG=1` makes `SpawnerManager` report what it
thinks is in its trigger box and how much of its link list has resolved, the
Fire action report which of throw/recall it chose and the four values that
decide it, and `GameManager::ResolvePlayers` report the player pairing - each
once per change rather than per tick. `VOXAGINE_CHUNK_IO_TIMINGS=1` gained two
`VoxelBaker` lines: why an already-stamped renderer is being stamped again
(which flag let it past the skip tests), and how many voxels a stamp wrote that
you can *see but not collide with*. **All three exist because the questions they
answer are unreachable from a headless run** - `--ui-script` can neither fire a
weapon nor walk into an arena - and each of them killed a wrong hypothesis of
mine within one run.

**Run the game headless. Never put a window on the user's display.** Joey works
on the same screen, and a compositor-sized window is not reproducible either -
Hyprland picks the size, so two runs of the same binary can differ 4x in pixel
count with nothing in the log saying so. That produced a Voxel pass reading
*below* the same build's shadow-less floor once, caught only because the
full-screen Post Processing pass moved with it.

```bash
cd Game && ../Build/Linux/Game/Release/bin/BitBuster \
  --hidden --size 2880x1620 --frames 900 \
  --map Content/Worlds/Castle/Valley_Path_To_Castle_Beat1.wld \
  --screenshot /tmp/shot.ppm
```

- `--hidden` renders into an *unmapped* SDL window: invisible, and not the
  compositor's to resize, so `--size` is honoured. `SDL_VIDEODRIVER=offscreen`
  does **not** work here - SDL needs `VK_EXT_headless_surface` for a Vulkan
  surface and this driver does not expose it; no Xvfb/cage/weston/sway either.
- `--size` is in *logical* units. `SDL_WINDOW_HIGH_PIXEL_DENSITY` scales it, so
  2880x1620 lands at 3840x2160.
- **`--map` replaces editing `ProjectSettings.vgps`.** Every phase note in
  `Docs/RENDERING_PLAN.md` before this carries a "reverted after capture" caveat,
  which is the tell. Shared config is churn another agent may be holding.
- `--screenshot` writes a binary PPM of any pass's target
  (`--screenshot-pass "Sun Shadow"` dumps the shadow map itself, which is how
  an intermediate gets inspected without writing a debug shader). Convert to
  PNG with Python's zlib.
- `--frames <n>` makes a run terminate itself instead of being killed on a
  timer. `LaunchOptions.h` is the reference. **It counts main-loop iterations,
  not frames the GPU finished** - and when the GPU is behind, `Present`
  early-returns and the loop spins, 4000 "frames" in 0.8 s. A capture taken
  that way is of the **startup fade**: a uniformly black image that looks
  exactly like a shading bug, and three different diagnostics returning wildly
  different values produced byte-identical image statistics before that was
  spotted. If a run printed no `[fps]` line, it produced no measurement and no
  valid capture. Cap expensive configurations with `timeout` and a huge
  `--frames` instead.

**RenderDoc can be driven from inside the process, and has to be here.**
`RenderDocCapture` (`Core/Platform/Rendering/RenderDocCapture.h`) loads
RenderDoc's in-application API and triggers captures by environment variable:
`VOXAGINE_RENDERDOC_CAPTURE=<n>` captures the first n presented frames,
`VOXAGINE_RENDERDOC=1` attaches without capturing, `VOXAGINE_RENDERDOC_PATH`
sets where they go. `TriggerCapture(n)` is callable from code, which is the
point - the interesting frame of a headless scripted run is one specific frame
three thousand frames in, and nobody is there to press F12. It must initialize
before `Platform::Initialize`, because RenderDoc hooks Vulkan through the
loader and the instance is created there.
**Absence is a no-op and that is the only half that is verified**: RenderDoc is
not installed on the dev machine, so the attached paths have never run. The ABI
is upstream's own `renderdoc_app.h`, vendored unmodified under
`Source/External/renderdoc/` (MIT), so what is untested is the calls rather
than the struct layout.

**Screenshots are expensive. Look once to confirm; *measure* to iterate.** Two
scalar metrics over a fixed crop - mean luminance, and the fraction of pixels
differing from *both* horizontal neighbours by more than a threshold - ranked
three candidate shadow builds correctly and caught a bug as a brightness jump
that no amount of looking at a penumbra would have named. Reach for an image at
a decision point, not once per attempt.

**Verify at the case that is hard, not the one that is convenient.** Three
shading regressions in a row got past a capture crop that was open ground; all
three lived in enclosed geometry. For `Valley_Path_To_Castle_Beat1` the crop
that matters is the left/right valley walls - `(60, 250, 900, 640)` of a
3840x2160 capture.

- **Ask the user to look at the screen** for judgement calls - how heavy AO
  should be, whether a penumbra reads. They see input response, flicker and
  behaviour over time that a still frame does not. Correctness and cost are
  yours to check headless first.
- Attaching a debugger to a running process needs `ptrace_scope`; launching
  *under* `gdb -nx -batch -ex run -ex "thread apply all bt"` works and catches
  the segfault with a full stack.
