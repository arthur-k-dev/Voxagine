#include "Core/System/MobileLog.h"

#if defined(VOXAGINE_ANDROID)

#include <android/log.h>

#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace
{
	const char* const k_pTag = "voxagine";

	int g_iPipe[2] = { -1, -1 };
	pthread_t g_Thread;

	void* PumpToLogcat(void*)
	{
		/* logcat truncates around 4 KB a line; the engine's longest line is the
		   [stall] diagnostic and it is nowhere near that. */
		char buffer[1024];
		size_t uiUsed = 0;

		for (;;)
		{
			const ssize_t iRead = read(g_iPipe[0], buffer + uiUsed, sizeof(buffer) - uiUsed - 1);

			if (iRead <= 0)
				break;

			uiUsed += static_cast<size_t>(iRead);
			buffer[uiUsed] = '\0';

			/* Split on newlines so logcat gets one entry per printf line rather
			   than per read(), which would otherwise glue unrelated messages
			   together whenever two arrive in the same buffer. */
			char* pLine = buffer;

			for (;;)
			{
				char* pNewline = strchr(pLine, '\n');

				if (pNewline == nullptr)
					break;

				*pNewline = '\0';
				__android_log_write(ANDROID_LOG_INFO, k_pTag, pLine);
				pLine = pNewline + 1;
			}

			/* Carry the unterminated tail into the next read. If a single line
			   ever exceeds the buffer, flush it rather than deadlocking on a
			   newline that will not come. */
			uiUsed = strlen(pLine);

			if (uiUsed >= sizeof(buffer) - 1)
			{
				__android_log_write(ANDROID_LOG_INFO, k_pTag, pLine);
				uiUsed = 0;
			}
			else if (uiUsed > 0)
			{
				memmove(buffer, pLine, uiUsed);
			}
		}

		return nullptr;
	}
}

namespace MobileLog
{
	void Install()
	{
		if (g_iPipe[0] != -1)
			return;

		/* Unbuffered, or the pipe fills with a frame's worth of output and
		   arrives in bursts long after the thing it describes. */
		setvbuf(stdout, nullptr, _IONBF, 0);
		setvbuf(stderr, nullptr, _IONBF, 0);

		if (pipe(g_iPipe) != 0)
			return;

		dup2(g_iPipe[1], STDOUT_FILENO);
		dup2(g_iPipe[1], STDERR_FILENO);

		if (pthread_create(&g_Thread, nullptr, PumpToLogcat, nullptr) != 0)
			return;

		pthread_detach(g_Thread);

		__android_log_write(ANDROID_LOG_INFO, k_pTag, "[log] stdout and stderr are on logcat");
	}
}

#else

namespace MobileLog
{
	void Install()
	{
		/* Desktop stdout already goes where the person running it can see it. */
	}
}

#endif
