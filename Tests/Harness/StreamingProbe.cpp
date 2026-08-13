#include "Harness/StreamingProbe.h"

#include <atomic>

#include <rttr/registration>

#include "Core/ECS/World.h"

namespace
{
	std::atomic<uint64_t> s_uiTicks{ 0 };
	std::atomic<uint64_t> s_uiFixedTicks{ 0 };
}

RTTR_REGISTRATION
{
	rttr::registration::class_<StreamingProbeEntity>("StreamingProbeEntity")
		.constructor<World*>()(rttr::policy::ctor::as_raw_ptr)
		.property("LinkTarget", &StreamingProbeEntity::GetLinkTarget, &StreamingProbeEntity::SetLinkTarget);
}

void StreamingProbeEntity::Tick(float fDeltaTime)
{
	Entity::Tick(fDeltaTime);

	s_uiTicks.fetch_add(1, std::memory_order_relaxed);
}

void StreamingProbeEntity::FixedTick(const GameTimer& /*fixedTimer*/)
{
	s_uiFixedTicks.fetch_add(1, std::memory_order_relaxed);
}

uint64_t StreamingProbeEntity::TicksTaken()
{
	return s_uiTicks.load(std::memory_order_relaxed);
}

uint64_t StreamingProbeEntity::FixedTicksTaken()
{
	return s_uiFixedTicks.load(std::memory_order_relaxed);
}

void StreamingProbeEntity::ResetCounters()
{
	s_uiTicks.store(0, std::memory_order_relaxed);
	s_uiFixedTicks.store(0, std::memory_order_relaxed);
}
