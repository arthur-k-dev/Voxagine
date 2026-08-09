#pragma once

#include "Core/Resources/Formats/SoundReference.h"

/* The concrete SoundReference the resource manager hands out.
 *
 * It is backend-independent: loading and freeing both go through the
 * AudioContext, so the same class serves miniaudio and the silent stub and
 * there is nothing to swap in CMake. That is a change from the FMOD era, where
 * two translation units defined the same class - FMODSoundReference.cpp called
 * FMOD::Sound::release() directly, so the class knew which backend it was for
 * and a silent build needed a second copy of it (NullSoundReference.cpp).
 *
 * Named for what it is rather than for a backend so the next backend does not
 * repeat that. */
class PlatformSoundReference : public SoundReference
{
public:
	PlatformSoundReference(const std::string& filePath) : SoundReference(filePath) {}
	virtual ~PlatformSoundReference();

	virtual bool Load(const std::string& filePath) override;
	virtual void Free() override;
};
