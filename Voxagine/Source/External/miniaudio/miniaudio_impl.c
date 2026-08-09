/* The single miniaudio implementation translation unit. See README.md for why
   stb_vorbis has to be declared before MINIAUDIO_IMPLEMENTATION is expanded.

   Compiled as C, so nothing here is affected by the engine's pch or its
   warning set. */

/* Nothing in this engine encodes audio, and the encoders are a meaningful
   slice of the build time of a 4 MB header. */
#define MA_NO_ENCODING

/* Declarations only - stb_vorbis.c is compiled as its own translation unit.
   Defining STB_VORBIS_INCLUDE_STB_VORBIS_H is what switches miniaudio's
   Vorbis decoder on. */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
