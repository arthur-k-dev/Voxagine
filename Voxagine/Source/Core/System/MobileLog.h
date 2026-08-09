#pragma once

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
 * A no-op everywhere else, where stdout already goes where it should.
 */

namespace MobileLog
{
	/* Call once, before anything prints - which means before
	   MobileAssets::PrepareAssetRoot, whose extraction progress is the first
	   thing worth seeing. */
	void Install();
}
