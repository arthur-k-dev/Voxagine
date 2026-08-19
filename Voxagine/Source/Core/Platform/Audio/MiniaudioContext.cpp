#include "pch.h"
#include "MiniaudioContext.h"

#include "../Platform.h"
#include "Core/Application.h"
#include "Core/LaunchOptions.h"
#include "Core/LoggingSystem/LoggingSystem.h"
#include "Core/Resources/Formats/SoundReference.h"

#include "External/miniaudio/miniaudio.h"

#include <cstdio>
#include <cstdlib>
#include <string>

/* Spatialisation falloff, in voxels, with an inverse model: full volume inside
   the min distance, 1/d beyond it, silent past the max.
 *
 * These are a starting point, not a measurement. FMOD ran on its own defaults
 * (min 1, max 10000), which on a world where a character is ~20 voxels tall
 * would have put every sound at 1% volume the moment it was a metre away - and
 * nobody ever heard it, because FMOD was never linked. Tune by ear; they are
 * the only two numbers in this backend that are a judgement call. */
static const float k_fMinAudibleDistance = 32.f;
static const float k_fMaxAudibleDistance = 512.f;

/* ------------------------------------------------------------------------- */

struct MiniaudioContext::Engine
{
	ma_context context;
	ma_engine engine;
	bool bContextInitialized = false;
	bool bInitialized = false;
};

/* One loaded asset. `master` is what SoundReference::Sound points at. */
struct MiniaudioContext::Sound
{
	ma_sound master;
	std::string path;
	bool bSpatial = true;
	bool bStreaming = false;
	bool bMasterValid = false;

	/* The file's *own* rate, not the engine's. Loop points in the .cfg
	   sidecars are in frames of this, and miniaudio's are in frames of the
	   engine's output rate - see ToEngineFrames. */
	ma_uint32 uiFileSampleRate = 0;
};

/* One playing (or paused) instance. Every channel owns its own ma_sound. */
struct MiniaudioContext::Channel
{
	ma_sound sound;
	bool bSpatial = true;
	ma_uint32 uiFileSampleRate = 0;
};

/* ------------------------------------------------------------------------- */

MiniaudioContext::MiniaudioContext(Platform* pPlatform) : AudioContext(pPlatform)
{
}

MiniaudioContext::~MiniaudioContext()
{
	/* Channels first: they hold references into the engine's resource manager
	   and uninitialising the engine under them is a use-after-free. */
	for (Channel* pChannel : m_Channels)
	{
		ma_sound_uninit(&pChannel->sound);
		delete pChannel;
	}

	m_Channels.clear();
	m_pBGMChannel = nullptr;

	if (m_pEngine != nullptr)
	{
		if (m_pEngine->bInitialized)
			ma_engine_uninit(&m_pEngine->engine);

		if (m_pEngine->bContextInitialized)
			ma_context_uninit(&m_pEngine->context);

		delete m_pEngine;
		m_pEngine = nullptr;
	}
}

void MiniaudioContext::LogError(const char* pWhat, int iResult) const
{
	if (m_pLoggingSystem == nullptr)
		return;

	m_pLoggingSystem->Log(LOGLEVEL_ERROR, "Audio",
		std::string(pWhat) + " (miniaudio result " + std::to_string(iResult) + ")");
}

void MiniaudioContext::Initialize()
{
	m_pEngine = new Engine();

	/* A hidden window means a headless run - a capture, a benchmark, a CI job -
	   and a headless run has no business taking the machine's sound device.
	   Every --hidden measurement taken before this played the game's music out
	   loud on whatever the developer was listening to.

	   It is an implication rather than a rule, so VOXAGINE_AUDIO_NULL_DEVICE
	   still decides when it is set, in *both* directions: `=0` alongside
	   --hidden gets a real device back (for confirming a sound by ear without a
	   window in the way), and `=1` without --hidden gets the null one. */
	m_bNullDevice = LaunchOptions::Get().IsHidden();

	if (const char* pNullDevice = std::getenv("VOXAGINE_AUDIO_NULL_DEVICE"))
		m_bNullDevice = pNullDevice[0] != '\0' && pNullDevice[0] != '0';

	ma_engine_config config = ma_engine_config_init();

	if (m_bNullDevice)
	{
		/* Mixes into nothing at a simulated clock. Everything above this line
		   behaves identically, which is the point: a headless run still proves
		   the .ogg files decode. */
		ma_backend backend = ma_backend_null;

		if (ma_context_init(&backend, 1, nullptr, &m_pEngine->context) == MA_SUCCESS)
		{
			m_pEngine->bContextInitialized = true;
			config.pContext = &m_pEngine->context;

			printf("[audio] mixing to the null device - nothing reaches a speaker\n");
		}
	}

	ma_result result = ma_engine_init(&config, &m_pEngine->engine);

	if (result != MA_SUCCESS)
	{
		/* A machine with no sound device is a normal state - CI runs headless
		   and so does every --hidden capture - so this is not fatal. Every
		   entry point below tests bInitialized and does nothing. */
		LogError("no audio device; running silent", (int)result);
		return;
	}

	m_pEngine->bInitialized = true;

	/* Y up, matching the engine's world axes. Listener orientation is fed per
	   frame from the camera by AudioSystem::PostTick. */
	ma_engine_listener_set_world_up(&m_pEngine->engine, 0, 0.f, 1.f, 0.f);
}

void MiniaudioContext::Update()
{
	/* miniaudio mixes on its own thread; there is no per-frame pump. */
}

void MiniaudioContext::PauseAll()
{
	if (m_pEngine == nullptr || !m_pEngine->bInitialized)
		return;

	/* Stops the whole engine's device, not each sound: every sound's own
	   playing/paused state is untouched underneath, which is what makes this
	   safe to pair with ResumeAll around an app backgrounding regardless of
	   what was already playing or already paused. The alternative - iterating
	   m_Channels and pausing each - would forget that state and resume
	   everything, including whatever the player had already paused. */
	ma_engine_stop(&m_pEngine->engine);
}

void MiniaudioContext::ResumeAll()
{
	if (m_pEngine == nullptr || !m_pEngine->bInitialized)
		return;

	ma_engine_start(&m_pEngine->engine);
}

MiniaudioContext::Channel* MiniaudioContext::Resolve(void* pChannel) const
{
	if (pChannel == nullptr)
		return nullptr;

	Channel* pCandidate = static_cast<Channel*>(pChannel);

	return m_Channels.find(pCandidate) != m_Channels.end() ? pCandidate : nullptr;
}

MiniaudioContext::Channel* MiniaudioContext::OpenChannel(Sound* pSound)
{
	if (m_pEngine == nullptr || !m_pEngine->bInitialized || pSound == nullptr)
		return nullptr;

	Channel* pChannel = new Channel();
	pChannel->bSpatial = pSound->bSpatial;
	pChannel->uiFileSampleRate = pSound->uiFileSampleRate;

	ma_result result;

	if (pSound->bStreaming)
	{
		/* A stream cannot be copied, so music opens the file again. That is
		   once per track change, not per frame. */
		result = ma_sound_init_from_file(&m_pEngine->engine, pSound->path.c_str(),
			MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION,
			nullptr, nullptr, &pChannel->sound);
	}
	else
	{
		result = ma_sound_init_copy(&m_pEngine->engine, &pSound->master, 0, nullptr, &pChannel->sound);
	}

	if (result != MA_SUCCESS)
	{
		LogError(("could not open a channel for " + pSound->path).c_str(), (int)result);
		delete pChannel;
		return nullptr;
	}

	if (pChannel->bSpatial)
	{
		/* FMOD ran with set3DSettings(0, 100, 1) - doppler off, inverse
		   rolloff. Doppler on a game whose camera teleports with the chunk
		   window is a pitch artefact, not an effect. */
		ma_sound_set_doppler_factor(&pChannel->sound, 0.f);
		ma_sound_set_attenuation_model(&pChannel->sound, ma_attenuation_model_inverse);
		ma_sound_set_min_distance(&pChannel->sound, k_fMinAudibleDistance);
		ma_sound_set_max_distance(&pChannel->sound, k_fMaxAudibleDistance);
	}
	else
	{
		ma_sound_set_spatialization_enabled(&pChannel->sound, MA_FALSE);
	}

	m_Channels.insert(pChannel);

	return pChannel;
}

void MiniaudioContext::CloseChannel(Channel* pChannel)
{
	if (pChannel == nullptr)
		return;

	if (m_pBGMChannel == pChannel)
		m_pBGMChannel = nullptr;

	ma_sound_uninit(&pChannel->sound);

	m_Channels.erase(pChannel);
	delete pChannel;
}

/* Loop points as this engine has always stored them: PCM frames at the *file's
   own* sample rate. That is what FMOD_TIMEUNIT_PCM meant, and it is what the
   .cfg sidecars next to every music asset were authored against.
 *
 * miniaudio's loop points are frames of the data source's *output* - after
 * resampling to the engine's rate - so passing the raw .cfg values through
 * unconverted loops at the wrong point whenever a track's native rate differs
 * from the engine's. A 44.1 kHz track through a 48 kHz engine loops at
 * 44100/48000 of the intended sample: about 8% early, and by a different
 * fraction for every track not already at the engine's rate - which is
 * exactly "the loop points do not match, track to track".
 *
 * The conversion is symmetric: multiply by engine/file going in (SetLoopPoints,
 * PlayBGM), by file/engine coming back out (GetLoopPoints). */
ma_uint64 MiniaudioContext::ToEngineFrames(ma_uint64 uiFileFrames, ma_uint32 uiFileSampleRate) const
{
	if (uiFileSampleRate == 0 || m_pEngine == nullptr || !m_pEngine->bInitialized)
		return uiFileFrames;

	const ma_uint32 uiEngineSampleRate = ma_engine_get_sample_rate(&m_pEngine->engine);

	if (uiEngineSampleRate == 0 || uiEngineSampleRate == uiFileSampleRate)
		return uiFileFrames;

	return static_cast<ma_uint64>(
		(static_cast<double>(uiFileFrames) * uiEngineSampleRate) / uiFileSampleRate + 0.5);
}

ma_uint64 MiniaudioContext::ToFileFrames(ma_uint64 uiEngineFrames, ma_uint32 uiFileSampleRate) const
{
	if (uiFileSampleRate == 0 || m_pEngine == nullptr || !m_pEngine->bInitialized)
		return uiEngineFrames;

	const ma_uint32 uiEngineSampleRate = ma_engine_get_sample_rate(&m_pEngine->engine);

	if (uiEngineSampleRate == 0 || uiEngineSampleRate == uiFileSampleRate)
		return uiEngineFrames;

	return static_cast<ma_uint64>(
		(static_cast<double>(uiEngineFrames) * uiFileSampleRate) / uiEngineSampleRate + 0.5);
}

bool MiniaudioContext::CreateSound(const std::string& soundPath, void*& pSound, bool bIs3D)
{
	pSound = nullptr;

	if (m_pEngine == nullptr || !m_pEngine->bInitialized)
		return false;

	Sound* pNew = new Sound();
	pNew->path = m_pPlatform->GetBasePath() + soundPath;
	pNew->bSpatial = bIs3D;

	/* Music is everything the rest of the engine already treats as music: the
	   "_BGM" naming convention reaches here as bIs3D == false. */
	pNew->bStreaming = !bIs3D;

	ma_uint32 flags = pNew->bStreaming
		? (MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION)
		: MA_SOUND_FLAG_DECODE;

	ma_result result = ma_sound_init_from_file(&m_pEngine->engine, pNew->path.c_str(),
		flags, nullptr, nullptr, &pNew->master);

	if (result != MA_SUCCESS)
	{
		LogError(("could not load " + pNew->path).c_str(), (int)result);
		delete pNew;
		return false;
	}

	pNew->bMasterValid = true;

	/* The file's native sample rate, read with a throwaway decoder.
	 *
	 * It cannot come from ma_sound_get_data_format: a resource-manager sound
	 * reports the rate it *outputs*, which is the engine's, and that is the
	 * trap this whole conversion exists to avoid. Opening the file a second
	 * time costs a header parse, once, at load. */
	{
		ma_decoder probe;

		if (ma_decoder_init_file(pNew->path.c_str(), nullptr, &probe) == MA_SUCCESS)
		{
			pNew->uiFileSampleRate = probe.outputSampleRate;
			ma_decoder_uninit(&probe);
		}
	}

	/* The master is a template, never played: effects copy it, music re-opens
	   the file. It exists so GetLength() has something to ask. */
	pSound = pNew;

	return true;
}

void MiniaudioContext::DestroySound(void*& pSound)
{
	if (pSound == nullptr)
		return;

	Sound* pTarget = static_cast<Sound*>(pSound);

	if (pTarget->bMasterValid)
		ma_sound_uninit(&pTarget->master);

	delete pTarget;
	pSound = nullptr;
}

void MiniaudioContext::PlaySound(const SoundReference* pSoundReference, void*& pChannel,
                                 const Vector3& v3Position, float fVolume, bool bIsPaused)
{
	if (pSoundReference == nullptr || pSoundReference->Sound == nullptr)
		return;

	/* An existing channel is a resume, matching the FMOD backend. */
	if (Channel* pExisting = Resolve(pChannel))
	{
		if (bIsPaused)
			ma_sound_stop(&pExisting->sound);
		else
			ma_sound_start(&pExisting->sound);

		return;
	}

	Channel* pNew = OpenChannel(static_cast<Sound*>(pSoundReference->Sound));

	if (pNew == nullptr)
	{
		pChannel = nullptr;
		return;
	}

	ma_sound_set_volume(&pNew->sound, std::max(0.f, fVolume));

	if (pNew->bSpatial)
		ma_sound_set_position(&pNew->sound, v3Position.x, v3Position.y, v3Position.z);

	if (!bIsPaused)
		ma_sound_start(&pNew->sound);

	pChannel = pNew;
}

void MiniaudioContext::PauseSound(void* pChannel)
{
	/* ma_sound_stop() is a pause - it leaves the cursor where it is, and
	   ma_sound_start() resumes from there. Rewinding is a seek. */
	if (Channel* pTarget = Resolve(pChannel))
		ma_sound_stop(&pTarget->sound);
}

void MiniaudioContext::StopSound(void* pChannel)
{
	/* Callers null their own handle straight after this, and every read path
	   validates against m_Channels, so freeing here is safe. */
	CloseChannel(Resolve(pChannel));
}

void MiniaudioContext::PlayBGM(SoundReference* pSoundReference, float fVolume,
                               uint32_t uiLoopStart, uint32_t uiLoopEnd)
{
	if (pSoundReference == nullptr || pSoundReference->Sound == nullptr || pSoundReference == m_pBGMReference)
		return;

	fVolume = std::max(0.f, fVolume);

	if (m_pBGMReference != nullptr)
		m_pBGMReference->Release();

	m_pBGMReference = pSoundReference;
	m_pBGMReference->IncrementRef();

	CloseChannel(m_pBGMChannel);

	m_pBGMChannel = OpenChannel(static_cast<Sound*>(pSoundReference->Sound));

	if (m_pBGMChannel == nullptr)
		return;

	ma_sound_set_looping(&m_pBGMChannel->sound, MA_TRUE);

	if (uiLoopEnd > uiLoopStart)
	{
		const ma_uint64 uiBegin = ToEngineFrames(uiLoopStart, m_pBGMChannel->uiFileSampleRate);
		const ma_uint64 uiEnd = ToEngineFrames(uiLoopEnd, m_pBGMChannel->uiFileSampleRate);

		ma_data_source_set_loop_point_in_pcm_frames(
			ma_sound_get_data_source(&m_pBGMChannel->sound), uiBegin, uiEnd);
	}

	ma_sound_set_volume(&m_pBGMChannel->sound, fVolume);
	ma_sound_start(&m_pBGMChannel->sound);

	m_fBGMVolume = fVolume;
}

void MiniaudioContext::ResumeBGM()
{
	if (m_pBGMChannel != nullptr)
		ma_sound_start(&m_pBGMChannel->sound);
}

void MiniaudioContext::PauseBGM()
{
	if (m_pBGMChannel != nullptr)
		ma_sound_stop(&m_pBGMChannel->sound);
}

void MiniaudioContext::StopBGM()
{
	CloseChannel(m_pBGMChannel);
	m_pBGMChannel = nullptr;

	/* Null when the reference is the thing being destroyed - see
	   AudioContext::OnReferenceDestroyed, which clears it first precisely so
	   this does not Release() an object already inside its own destructor. */
	if (m_pBGMReference != nullptr)
	{
		SoundReference* pReference = m_pBGMReference;
		m_pBGMReference = nullptr;
		pReference->Release();
	}
}

void MiniaudioContext::SetBGMVolume(float fVolume)
{
	fVolume = std::max(0.f, fVolume);

	if (m_pBGMChannel != nullptr)
		ma_sound_set_volume(&m_pBGMChannel->sound, fVolume);

	m_fBGMVolume = fVolume;
}

bool MiniaudioContext::IsBGMPlaying() const
{
	return m_pBGMChannel != nullptr && ma_sound_is_playing(&m_pBGMChannel->sound) == MA_TRUE;
}

void* MiniaudioContext::GetBGMChannel()
{
	return m_pBGMChannel;
}

bool MiniaudioContext::IsPlaying(void* pChannel)
{
	Channel* pTarget = Resolve(pChannel);

	return pTarget != nullptr && ma_sound_is_playing(&pTarget->sound) == MA_TRUE;
}

float MiniaudioContext::GetLength(const SoundReference* pSoundReference)
{
	if (pSoundReference == nullptr || pSoundReference->Sound == nullptr)
		return 0.f;

	Sound* pSound = static_cast<Sound*>(pSoundReference->Sound);

	if (!pSound->bMasterValid)
		return 0.f;

	float fLength = 0.f;
	ma_sound_get_length_in_seconds(&pSound->master, &fLength);

	return fLength;
}

float MiniaudioContext::GetPlaybackPosition(void* pChannel)
{
	Channel* pTarget = Resolve(pChannel);

	if (pTarget == nullptr)
		return 0.f;

	float fCursor = 0.f;
	ma_sound_get_cursor_in_seconds(&pTarget->sound, &fCursor);

	return fCursor;
}

float MiniaudioContext::GetVolume(void* pChannel) const
{
	Channel* pTarget = Resolve(pChannel);

	return pTarget != nullptr ? ma_sound_get_volume(&pTarget->sound) : 1.f;
}

void MiniaudioContext::SetVolume(void* pChannel, float fVolume)
{
	if (Channel* pTarget = Resolve(pChannel))
		ma_sound_set_volume(&pTarget->sound, std::max(0.f, fVolume));
}

void MiniaudioContext::SetPlaybackPosition(void* pChannel, float position)
{
	if (Channel* pTarget = Resolve(pChannel))
		ma_sound_seek_to_second(&pTarget->sound, std::max(0.f, position));
}

void MiniaudioContext::Set3DSystemParameters(const Vector3& v3Position, const Vector3& v3Velocity,
                                             const Vector3& v3Forward, const Vector3& v3Up)
{
	if (m_pEngine == nullptr || !m_pEngine->bInitialized)
		return;

	ma_engine_listener_set_position(&m_pEngine->engine, 0, v3Position.x, v3Position.y, v3Position.z);
	ma_engine_listener_set_velocity(&m_pEngine->engine, 0, v3Velocity.x, v3Velocity.y, v3Velocity.z);
	ma_engine_listener_set_direction(&m_pEngine->engine, 0, v3Forward.x, v3Forward.y, v3Forward.z);
	ma_engine_listener_set_world_up(&m_pEngine->engine, 0, v3Up.x, v3Up.y, v3Up.z);
}

void MiniaudioContext::Set3DParameters(void* pChannel, Vector3 position, Vector3 velocity)
{
	Channel* pTarget = Resolve(pChannel);

	if (pTarget == nullptr || !pTarget->bSpatial)
		return;

	ma_sound_set_position(&pTarget->sound, position.x, position.y, position.z);
	ma_sound_set_velocity(&pTarget->sound, velocity.x, velocity.y, velocity.z);
}

void MiniaudioContext::GetLoopPoints(void* pChannel, uint32_t& uiLoopStart, uint32_t& uiLoopEnd) const
{
	uiLoopStart = 0;
	uiLoopEnd = 0;

	Channel* pTarget = Resolve(pChannel);

	if (pTarget == nullptr)
		return;

	ma_uint64 uiBegin = 0;
	ma_uint64 uiEnd = 0;

	ma_data_source_get_loop_point_in_pcm_frames(
		ma_sound_get_data_source(&pTarget->sound), &uiBegin, &uiEnd);

	uiLoopStart = static_cast<uint32_t>(ToFileFrames(uiBegin, pTarget->uiFileSampleRate));
	uiLoopEnd = static_cast<uint32_t>(ToFileFrames(uiEnd, pTarget->uiFileSampleRate));
}

void MiniaudioContext::SetLoopPoints(void* pChannel, uint32_t uiLoopStart, uint32_t uiLoopEnd)
{
	Channel* pTarget = Resolve(pChannel);

	if (pTarget == nullptr || uiLoopEnd <= uiLoopStart)
		return;

	const ma_uint64 uiBegin = ToEngineFrames(uiLoopStart, pTarget->uiFileSampleRate);
	const ma_uint64 uiEnd = ToEngineFrames(uiLoopEnd, pTarget->uiFileSampleRate);

	ma_data_source_set_loop_point_in_pcm_frames(
		ma_sound_get_data_source(&pTarget->sound), uiBegin, uiEnd);
}

void MiniaudioContext::SetLoopCount(void* pChannel, int32_t iLoopCount)
{
	/* miniaudio loops or it does not; a finite repeat count has no equivalent
	   and nothing in the game asks for one - AudioSystem passes either the
	   source's count or 0, and every non-zero count in the content is -1. */
	if (Channel* pTarget = Resolve(pChannel))
		ma_sound_set_looping(&pTarget->sound, iLoopCount != 0 ? MA_TRUE : MA_FALSE);
}
