#pragma once

/* Making a packaged app look like a directory of files.
 *
 * Every asset path in this engine is a bare relative string handed to fopen -
 * "Content/Music/Arena_BGM.ogg", "Engine/Assets/Shaders/Particles.vs.spv" -
 * resolved against the process's working directory. There are on the order of a
 * hundred such sites and no central resolver to hook. Run from a terminal in
 * Game/ that is fine: everything is where it says it is.
 *
 * Nothing that ships as an application bundle gives you that for free:
 *
 *  - **Android** assets are entries in a zip. `fopen` cannot see them at all;
 *    they need AAssetManager. Nothing in this engine speaks AAssetManager.
 *  - **iOS** assets *are* real files in the app bundle, but the bundle is
 *    read-only and the working directory is not it.
 *  - **macOS** has the same problem the moment anything is double-clicked
 *    rather than run from a shell: a bundle launched from Finder inherits a
 *    working directory of "/", so every relative path above resolves against
 *    the root of the disk. This is why the editor - which is a .app there -
 *    could not previously be launched the way a Mac application is launched.
 *    The same binary run from Game/ is not bundled and is left alone; only the
 *    runtime can tell those two apart, so the check is a runtime one.
 *
 * So the asset tree is copied out once, on first launch, into the app's private
 * writable directory, and the process chdir()s there. Every existing relative
 * path then resolves, unmodified, including the ones the engine *writes* -
 * Settings.vgs, PlayerPrefs. That is the whole trick, and it is deliberately
 * the boring option: rewriting a hundred call sites to go through a resolver
 * is a much larger change with a much worse failure mode (the one site nobody
 * converted).
 *
 * **It costs a second copy of the assets on disk** - ~100 MB here - and one
 * slow first launch. Both are recorded in MOBILE_PORT_LOG.md as the price of
 * not touching those call sites, and both are revisitable if a device says
 * they matter. A game build avoids the copy where it can: on iOS and macOS the
 * two read-only trees are symlinked back into the bundle instead. An editor
 * build cannot, because it writes into them.
 *
 * Re-extraction is skipped when a stamp file matching VOXAGINE_ASSET_VERSION is
 * already present, so it happens once per install rather than once per launch.
 *
 * Setting **VOXAGINE_ASSET_ROOT** in the environment overrides all of this and
 * uses that directory as-is. That is the developer escape hatch for the macOS
 * editor: without it, worlds saved from Voxagine.app go to a private container
 * rather than to the Game/ tree they came from.
 */

namespace MobileAssets
{
	/* Prepares and enters the asset root. Call before anything reads a file -
	   in practice, first thing in main().

	   Returns true if the working directory now points at a usable asset tree,
	   or if the platform needed nothing doing (desktop, where this is a no-op
	   and always returns true). A false return means the app cannot find its
	   content and should say so rather than starting. */
	bool PrepareAssetRoot();
}
