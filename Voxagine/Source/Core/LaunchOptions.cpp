#include "pch.h"

#include "Core/LaunchOptions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

LaunchOptions& LaunchOptions::Get()
{
	static LaunchOptions instance;
	return instance;
}

namespace
{
	/* "1920x1080" -> (1920, 1080). Returns false on anything else. */
	bool ParseSize(const char* pText, uint32_t& uiWidth, uint32_t& uiHeight)
	{
		const char* pSeparator = strchr(pText, 'x');

		if (pSeparator == nullptr)
			return false;

		const long lWidth = strtol(pText, nullptr, 10);
		const long lHeight = strtol(pSeparator + 1, nullptr, 10);

		if (lWidth <= 0 || lHeight <= 0)
			return false;

		uiWidth = static_cast<uint32_t>(lWidth);
		uiHeight = static_cast<uint32_t>(lHeight);

		return true;
	}
}

bool LaunchOptions::Parse(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i)
	{
		const bool bHasValue = (i + 1) < argc;

		if (strcmp(argv[i], "--map") == 0 && bHasValue)
		{
			m_Map = argv[++i];
		}
		else if (strcmp(argv[i], "--frames") == 0 && bHasValue)
		{
			m_uiFrames = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
		}
		else if (strcmp(argv[i], "--hidden") == 0)
		{
			m_bHidden = true;
		}
		else if (strcmp(argv[i], "--size") == 0 && bHasValue)
		{
			if (!ParseSize(argv[++i], m_uiWidth, m_uiHeight))
				fprintf(stderr, "[args] --size wants WxH, e.g. 1920x1080\n");
		}
		else if (strcmp(argv[i], "--screenshot") == 0 && bHasValue)
		{
			m_Screenshot = argv[++i];
		}
		else if (strcmp(argv[i], "--screenshot-pass") == 0 && bHasValue)
		{
			m_ScreenshotPass = argv[++i];
		}
		else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
		{
			printf(
				"Voxagine / Bit Buster\n"
				"\n"
				"  --map <path>            world to load instead of ProjectSettings' DefaultMap\n"
				"  --frames <n>            run n frames and exit; 0 (default) runs until closed\n"
				"  --hidden                create the window unmapped - renders, displays nothing\n"
				"  --size <WxH>            window size. Only honoured with --hidden; a mapped\n"
				"                          window is the compositor's to size\n"
				"  --screenshot <path>     write the last frame as a binary PPM and exit\n"
				"  --screenshot-pass <name>  which pass's target to capture\n"
				"                          (default \"Post Processing\"; try \"Voxel\", \"Sun Shadow\")\n"
				"\n"
				"A benchmark or a look, without touching any file or taking the display:\n"
				"  BitBuster --hidden --size 3840x2160 --frames 600 --map Content/Worlds/...\n");

			return false;
		}
		else
		{
			fprintf(stderr, "[args] ignoring unrecognised argument '%s'\n", argv[i]);
		}
	}

	/* A capture with no frame limit would never fire: the capture happens on
	   the last frame. Default to one long enough for a world to finish
	   streaming in rather than silently doing nothing. */
	if (!m_Screenshot.empty() && m_uiFrames == 0)
	{
		m_uiFrames = 240;
		fprintf(stderr, "[args] --screenshot with no --frames; defaulting to %u\n", m_uiFrames);
	}

	return true;
}
