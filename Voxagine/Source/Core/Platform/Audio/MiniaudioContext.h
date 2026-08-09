#pragma once

#include "Core/Platform/Audio/AudioContext.h"

#include <unordered_set>

/* AudioContext on miniaudio (Voxagine/Source/External/miniaudio).
 *
 * Replaces FMOD, which was a proprietary binary-only SDK that this tree never
 * actually linked - VOXAGINE_ENABLE_FMOD compiled the sources against vendored
 * headers and there was no library anywhere to link them to, so every platform
 * ran silent through NullAudioContext.
 *
 * Two shapes of sound, decided by the same `bIs3D` flag SoundReference already
 * passes (it is `path.find("_BGM") == npos`, the convention this codebase
 * already uses to tell music from effects):
 *
 *  - effects are decoded once into memory and every channel is a cheap copy of
 *    that decoded buffer (ma_sound_init_copy),
 *  - music is streamed from disk. A five-minute .ogg decodes to ~115 MB of f32
 *    PCM and there are 12 of them; decoding music is not affordable on a phone
 *    and is not free on a desktop either.
 *
 * VOXAGINE_AUDIO_NULL_DEVICE=1 runs the whole backend on miniaudio's null
 * device: sounds still load and decode, the mixer still runs, nothing reaches
 * a speaker. That is what a headless capture or a CI run wants - it exercises
 * the audio path rather than skipping it, which AA_NONE does.
 *
 * Channel handles handed out through the void* in the interface are validated
 * against m_Channels before use. The engine's channel bookkeeping is loose -
 * AudioSystem nulls its pointers after StopSound but AudioSource keeps its own
 * copies - and a stale handle here would be a use-after-free rather than the
 * no-op FMOD's internally-validated handles produced. */
class MiniaudioContext : public AudioContext
{
public:
	MiniaudioContext(Platform* pPlatform);
	virtual ~MiniaudioContext();

	virtual void Initialize() override;
	virtual void Update() override;

	virtual void PauseAll() override;
	virtual void ResumeAll() override;

	virtual bool CreateSound(const std::string& soundPath, void*& pSound, bool bIs3D = true) override;
	virtual void DestroySound(void*& pSound) override;

	virtual void PlaySound(const SoundReference* pSoundReference, void*& pChannel,
	                       const Vector3& v3Position = Vector3(0.f), float fVolume = 1.0f,
	                       bool bIsPaused = false) override;

	virtual void PauseSound(void* pChannel) override;
	virtual void StopSound(void* pChannel) override;

	virtual void PlayBGM(SoundReference* pSoundReference, float fVolume,
	                     uint32_t uiLoopStart, uint32_t uiLoopEnd) override;

	virtual void ResumeBGM() override;
	virtual void PauseBGM() override;
	virtual void StopBGM() override;

	virtual void SetBGMVolume(float fVolume) override;
	virtual bool IsBGMPlaying() const override;

	virtual void* GetBGMChannel() override;

	virtual bool IsPlaying(void* pChannel) override;

	virtual float GetLength(const SoundReference* pSoundReference) override;
	virtual float GetPlaybackPosition(void* pChannel) override;

	virtual float GetVolume(void* pChannel) const override;
	virtual void SetVolume(void* pChannel, float fVolume) override;

	virtual void SetPlaybackPosition(void* pChannel, float position) override;

	virtual void Set3DSystemParameters(const Vector3& v3Position, const Vector3& v3Velocity,
	                                   const Vector3& v3Forward, const Vector3& v3Up) override;

	virtual void Set3DParameters(void* pChannel, Vector3 position,
	                             Vector3 velocity = Vector3(0.f)) override;

	virtual void GetLoopPoints(void* pChannel, uint32_t& uiLoopStart, uint32_t& uiLoopEnd) const override;
	virtual void SetLoopPoints(void* pChannel, uint32_t uiLoopStart, uint32_t uiLoopEnd) override;

	virtual void SetLoopCount(void* pChannel, int32_t iLoopCount) override;

private:
	/* Defined in the .cpp; miniaudio.h stays out of every other translation
	   unit in the tree. */
	struct Engine;
	struct Channel;
	struct Sound;

	Channel* Resolve(void* pChannel) const;
	Channel* OpenChannel(Sound* pSound);
	void CloseChannel(Channel* pChannel);

	void LogError(const char* pWhat, int iResult) const;

	/* Loop-point sample-rate conversion - see the definition in the .cpp.
	 *
	 * `unsigned long long` rather than a stdint type or ma_uint64, and this is
	 * not a style choice: without MA_USE_STDINT (not defined anywhere in this
	 * build - see miniaudio.h) ma_uint64 is `unsigned long long`, which on an
	 * LP64 target (every platform here except Windows) is a *different type*
	 * from uint64_t's `unsigned long` despite both being 64 bits. A header
	 * declaration in one and a .cpp definition in the other do not match, and
	 * this is a genuinely non-obvious compile error to land on - "no
	 * declaration matches" with two identical-looking 64-bit types. */
	unsigned long long ToEngineFrames(unsigned long long uiFileFrames, unsigned int uiFileSampleRate) const;
	unsigned long long ToFileFrames(unsigned long long uiEngineFrames, unsigned int uiFileSampleRate) const;

	Engine* m_pEngine = nullptr;
	bool m_bNullDevice = false;
	Channel* m_pBGMChannel = nullptr;

	std::unordered_set<Channel*> m_Channels;
};
