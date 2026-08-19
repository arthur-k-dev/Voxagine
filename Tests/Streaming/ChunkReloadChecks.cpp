#include "Framework/Check.h"
#include "Harness/StreamingHarness.h"

#include "Core/ECS/Entity.h"
#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"

/* What survives a chunk leaving the window and coming back. M7 of
 * CHUNK_STREAMING_PLAN.md, reported as "destroyed terrain comes back when its
 * chunk reloads".
 *
 * A destroyed voxel has no carrier but the chunk volume: a VoxRenderer
 * serializes a model path and a transform, so the RootEntities JSON a chunk
 * writes on unload cannot hold damage even in principle. The encoded voxel blob
 * is the only candidate, and it is checked below - but the blob turned out to
 * be innocent. What actually puts the model back is the *reload* asking for a
 * re-stamp, and asking for it through a route the two existing guards do not
 * cover:
 *
 *   - `RenderSystem::OnComponentAdded` skips a renderer whose
 *     `IsChunkInstanceLoaded()` is set, and `Chunk::LoadEntities` sets that for
 *     every renderer of a non-first load. That guard holds.
 *   - `VoxelBaker::Bake` consults the same flag, but only as
 *     `(!Updated || bIsStaticChunkLoaded)` - **anded** with
 *     `!UpdateRequested()`. An explicit update request walks straight past it.
 *
 * And a reload does request one. `Chunk::SaveAndDeleteEntities` re-serializes
 * the departing entity from the *current* reflection registration, so the JSON
 * a chunk carries in memory has every property this build knows about even when
 * the level on disk predates them - and `VoxRenderer`'s "Emissive" setter calls
 * `RequestUpdate()` unconditionally, changed value or not. So the first unload
 * adds "Emissive" to the stored root, and every reload after it hands the baker
 * a renderer that says it wants re-stamping.
 *
 * There is no `RenderSystem` here, so this file checks the *request* rather than
 * the resulting voxels: no static renderer restored from chunk storage may ask
 * to be re-stamped. The consequence is checked in the game by
 * `StreamingCounters::ChunkInstanceRestamps`, which must stay zero across a real
 * traversal. That division is deliberate and is the same one the destruction
 * suite makes - harness for the decision, in-game audit for the pixels.
 */

namespace
{
	Vector3 CameraInColumn(uint32_t uiColumn)
	{
		return Vector3(uiColumn * 32.f + 16.f, 0.f, 16.f);
	}

	const Vector3 k_v3AtOffsetZero = CameraInColumn(1);
	const Vector3 k_v3AtOffset32 = CameraInColumn(2);

	/* Chunk column 0 is what leaves when the window slides one column right, so
	   Prop_0_0 is a static renderer that genuinely round-trips through
	   serialization and the RLE. */
	const std::string k_sDepartingProp = "Prop_0_0";

	VoxRenderer* PropRenderer(StreamingHarness& harness)
	{
		Entity* pEntity = harness.FindEntityNamed(k_sDepartingProp);

		return pEntity != nullptr ? pEntity->GetComponent<VoxRenderer>() : nullptr;
	}
}

VOXAGINE_CHECK(Streaming, AReloadedStaticRendererDoesNotAskToBeRestamped)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	{
		VoxRenderer* pRenderer = PropRenderer(harness);
		REQUIRE_TRUE(pRenderer != nullptr) << "the fixture's static prop was never admitted";
		CHECK_FALSE(pRenderer->IsChunkInstanceLoaded())
			<< "a first load is not a chunk instance restore";

		/* The fixture's stored VoxRenderer carries no "Emissive" member, exactly
		   as every shipped level does, so a first load never reaches the setter
		   that requests an update. This is half the mechanism: the difference
		   between the two loads is not the reload path, it is that the *chunk*
		   re-serialized the renderer from this build's registration. */
		CHECK_FALSE(pRenderer->UpdateRequested())
			<< "a first-load static renderer already wants re-stamping, so the reload "
			   "below would prove nothing";
	}

	/* Out, so the chunk unloads and re-serializes its roots from this build's
	   reflection registration, and back, so they are deserialized again. */
	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	CHECK_TRUE(PropRenderer(harness) == nullptr)
		<< "the departing chunk's static prop was not taken away with it";

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	VoxRenderer* pRenderer = PropRenderer(harness);
	REQUIRE_TRUE(pRenderer != nullptr) << "the returning chunk did not bring its prop back";

	CHECK_TRUE(pRenderer->IsChunkInstanceLoaded())
		<< "a renderer restored from chunk storage must say so, or nothing downstream can";

	/* The whole of M7 in one line. A renderer that asks for an update is
	   re-stamped from its pristine model over voxels that were decoded with
	   their damage in them. */
	CHECK_FALSE(pRenderer->UpdateRequested())
		<< "a chunk-restored static renderer asked to be re-stamped, which overwrites "
		   "the decoded voxels with the pristine model - this is M7";

	CHECK_FALSE(pRenderer->IsFrameChanged())
		<< "a chunk-restored renderer reports a changed frame, which reaches the baker "
		   "as BakeData::Updated";
}

VOXAGINE_CHECK(Streaming, DamageSurvivesAnUnloadAndReload)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	/* The chunk's own ground row, in window slot (0, *, 0) - chunk column 0,
	   which is what leaves. Clearing a ground voxel is exactly the shape of
	   destruction: colour zeroed, the storage left where it is. */
	const uint32_t uiGroundColor = harness.Grid().GetCell(5, 0, 5).GetColor();
	REQUIRE_TRUE(uiGroundColor != 0u) << "the ground voxel to damage is not occupied";

	{
		const VoxelCell cell = harness.Grid().GetCell(5, 0, 5);
		REQUIRE_TRUE(cell) << "the damaged cell is not resident to begin with";
		cell.SetColor(0);
	}

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	const VoxelCell cell = harness.Grid().GetCell(5, 0, 5);
	REQUIRE_TRUE(cell) << "the chunk did not come back";

	CHECK_EQ(cell.GetColor(), 0u)
		<< "a destroyed voxel came back through the unload/reload path";

	const UVector3 v3Size = harness.WindowSize();
	const uint32_t uiWordID = 5 + 0 * v3Size.x + 5 * v3Size.x * v3Size.y;

	CHECK_EQ(harness.Window().FrontWords()[uiWordID], 0u)
		<< "the republished window put the destroyed voxel back";

	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u);
}
