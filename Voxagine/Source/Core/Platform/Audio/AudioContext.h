#pragma once

#include "Core/Math.h"
#include <string>
#include <algorithm>

class Platform;
class LoggingSystem;
class SoundReference;

class AudioContext
{
public:
	AudioContext(Platform* pPlatform);
	virtual ~AudioContext() {}

	virtual void Initialize() = 0;
	virtual void Update() = 0;

	/* Every playing sound at once, without touching each one's own
	   play/pause state - a sound paused before the app backgrounded must stay
	   paused after PauseAll/ResumeAll, not resume because the pair around it
	   assumed nothing else was going on.
	 *
	 * Exists for one caller: the app going to the background. Nothing on
	 * desktop calls it - there is no "background" there - so the default does
	 * nothing, which is correct for NullAudioContext too. */
	virtual void PauseAll() {}
	virtual void ResumeAll() {}

	virtual bool CreateSound(const std::string& soundPath, void*& pSound, bool bIs3D = true) = 0;

	/* Releases what CreateSound produced and nulls the handle. The backend
	   owns the object behind SoundReference::Sound, so freeing it is the
	   context's job rather than the reference's - which is what let
	   PlatformSoundReference become backend-independent. */
	virtual void DestroySound(void*& pSound) = 0;

	virtual void PlaySound(const SoundReference* pSoundReference, void*& pChannel, const Vector3& v3Position = Vector3(0.f), float fVolume = 1.0f, bool bIsPaused = false) = 0;
	virtual void PauseSound(void* pChannel) = 0;
	virtual void StopSound(void* pChannel) = 0;

	virtual void PlayBGM(SoundReference* pSoundReference, float fVolume, uint32_t uiLoopStart, uint32_t uiLoopEnd) = 0;
	virtual void ResumeBGM() = 0;
	virtual void PauseBGM() = 0;
	virtual void StopBGM() = 0;

	float GetBGMVolume() const { return m_fBGMVolume; };
	virtual void SetBGMVolume(float fVolume) = 0;

	virtual bool IsBGMPlaying() const = 0;

	virtual void* GetBGMChannel() = 0;
	virtual SoundReference* GetBGMReference() const { return m_pBGMReference; };

	/* Called by a SoundReference as it is destroyed. The context holds raw
	   pointers to references it is playing, and a world swap frees the old
	   world's sounds out from under it - after which AudioSystem::PostTick
	   reads GetBGMReference()->GetRefPath() straight into freed memory.

	   The reference is cleared *before* StopBGM, not by it: a backend that
	   stops the music also drops its reference count, and Release() on an
	   object already inside its own destructor re-enters ReferenceManager and
	   frees it a second time. So StopBGM must tolerate a null reference, and
	   this path deliberately leaves that count alone - the object is going
	   away regardless. */
	virtual void OnReferenceDestroyed(SoundReference* pReference)
	{
		if (pReference != nullptr && m_pBGMReference == pReference)
		{
			m_pBGMReference = nullptr;
			StopBGM();
		}
	}

	virtual bool IsPlaying(void* pChannel) { return false; };

	virtual float GetLength(const SoundReference* pSoundReference) = 0;
	virtual float GetPlaybackPosition(void* pChannel) = 0;

	virtual float GetVolume(void* pChannel) const = 0;
	virtual void SetVolume(void* pChannel, float fVolume) = 0;

	virtual void SetPlaybackPosition(void* pChannel, float position) = 0;

	virtual void Set3DSystemParameters(const Vector3& v3Position, const Vector3& v3Velocity, const Vector3& v3Forward, const Vector3& v3Up) = 0;
	virtual void Set3DParameters(void* pChannel, Vector3 position, Vector3 velocity = Vector3(0.f)) = 0;

	virtual void GetLoopPoints(void* pChannel, uint32_t& uiLoopStart, uint32_t& uiLoopEnd) const = 0;
	virtual void SetLoopPoints(void* pChannel, uint32_t uiLoopStart, uint32_t uiLoopEnd) = 0;

	virtual void SetLoopCount(void* pChannel, int32_t iLoopCount) = 0;

protected:
	Platform* m_pPlatform = nullptr;
	LoggingSystem* m_pLoggingSystem = nullptr;

	float m_fBGMVolume = 1.f;

	SoundReference* m_pBGMReference = nullptr;
};