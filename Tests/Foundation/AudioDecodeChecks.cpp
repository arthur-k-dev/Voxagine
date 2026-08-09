#include "Framework/Check.h"

#include <string>

/* Proof that the audio backend can decode what this game actually ships.
 *
 * All 68 sound assets are Ogg Vorbis, and Vorbis is the one format miniaudio
 * does *not* compile in by default - it needs stb_vorbis declared before
 * MINIAUDIO_IMPLEMENTATION expands (see External/miniaudio/README.md). Drop
 * that wiring and nothing breaks loudly: ma_decoder_init_file returns
 * MA_NO_BACKEND, SoundReference::Load reports the same failure it reports for
 * a missing file, and the game runs silent exactly as it did for the whole
 * port. This check is the thing that would notice.
 *
 * It decodes rather than plays: no device is opened, so it runs on a headless
 * CI runner and makes no sound on a workstation. */

#if defined(VOXAGINE_MINIAUDIO) && defined(VOXAGINE_TEST_CONTENT_DIR)

#include "External/miniaudio/miniaudio.h"

namespace
{
	/* One of the shipped BGM tracks. Any .ogg would do; this one is small. */
	const char* k_pVorbisAsset = VOXAGINE_TEST_CONTENT_DIR "/Content/Music/Lose_BGM.ogg";
}

VOXAGINE_CHECK(AudioDecode, TheShippedVorbisAssetsDecode)
{
	ma_decoder decoder;

	ma_result result = ma_decoder_init_file(k_pVorbisAsset, nullptr, &decoder);

	REQUIRE_EQ((int)result, (int)MA_SUCCESS)
		<< "could not open " << k_pVorbisAsset
		<< " - MA_NO_BACKEND here means the Vorbis decoder is not compiled in";

	ma_uint64 uiFrames = 0;
	ma_data_source_get_length_in_pcm_frames(&decoder, &uiFrames);

	CHECK_GT(uiFrames, (ma_uint64)0) << "decoded a zero-length stream";
	CHECK_GT(decoder.outputSampleRate, 0u);
	CHECK_GT(decoder.outputChannels, 0u);

	/* Pull real samples. A decoder that opens but cannot produce frames is
	   the failure mode a format-detection regression actually has. */
	float samples[512];
	ma_uint64 uiRead = 0;

	result = ma_data_source_read_pcm_frames(&decoder, samples,
		sizeof(samples) / sizeof(samples[0]) / decoder.outputChannels, &uiRead);

	CHECK_EQ((int)result, (int)MA_SUCCESS);
	CHECK_GT(uiRead, (ma_uint64)0) << "decoder produced no PCM frames";

	ma_decoder_uninit(&decoder);
}

#endif
