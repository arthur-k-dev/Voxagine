#include <string>

#include "Framework/Invariant.h"

/* Rule 3: a voxel exists in six places - the CPU colour, the mapped GPU word,
   one bit of the occupancy bitmap, one unit of a brick count, the owner slot
   and the loose-voxel registry - and a write maintains all of them. A path that
   updates some is the whole reason VoxelEditBatch exists. */
class RepresentationsAgree : public Invariant
{
public:
	const char* Name() const override { return "representations-agree"; }

	std::string Check(const DestructionResult& result) const override
	{
		if (result.uiRepresentation == 0)
			return std::string();

		return std::to_string(result.uiRepresentation) +
			" voxels disagree between the mapped words, the occupancy bitmap and the brick counts";
	}
};

VOXAGINE_INVARIANT(RepresentationsAgree)
