#pragma once

#include <string>

/* Making the engine's own output visible on a phone.
 *
 * Android discards stdout and stderr. Not redirects - discards: a process's
 * fd 1 and 2 point at /dev/null unless something has been done about it, so
 * every printf and fprintf in this engine writes into nothing. That is the
 * whole diagnostic channel gone, on the one platform with no console, no argv
 * and no debugger attached by default. The first Android run looked like a
 * silent hang for exactly this reason - the app was fine and simply had no way
 * to say so.
 *
 * The fix is a pipe: dup stdout and stderr onto it, and pump the read end into
 * __android_log_write from a thread. Every existing call site is captured with
 * no changes, which is the same trade MobileAssets makes - a hundred untouched
 * call sites beats a hundred converted ones and the one that got missed.
 *
 * LoggingSystem also uses this as its platform sink. That keeps the engine's
 * structured diagnostics useful before an editor console exists: Android gets
 * logcat, iOS gets the unified device log, and desktop gets stderr.
 */

namespace MobileLog
{
	/* Call once, before anything prints - which means before
	   MobileAssets::PrepareAssetRoot, whose extraction progress is the first
	   thing worth seeing. */
	void Install();

	/* Publish one already-formatted engine log line to the platform's normal
	   diagnostic stream. This intentionally lives below LoggingSystem: callers
	   create ordinary LogEvents and do not need platform-specific logging code. */
	void Write(const std::string& message);
}
