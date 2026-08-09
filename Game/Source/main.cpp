#include "VoxApp.h"

#include "Core/LaunchOptions.h"

/* SDL owns the entry point on every platform now, so the wWinMain variant and
   its <eventtoken.h> include are gone.

   SDL_main.h is what makes that true rather than merely intended: on Android
   and iOS the platform calls into SDL first and SDL calls *this* - so the
   header renames main() to SDL_main() there. On Linux and Windows it expands
   to nothing, which is why it can be included unconditionally. Without it the
   Android build links a libmain.so whose entry point nothing ever calls, and
   the app starts to a black screen with no error. */
#include <SDL3/SDL_main.h>

#include "Core/System/MobileAssets.h"
#include "Core/System/MobileLog.h"

#include <SDL3/SDL_hints.h>

#include <cstdio>

int main(int argc, char* argv[])
{
	/* First of all, because everything below prints: Android throws stdout and
	   stderr away, so without this the whole engine is mute on the one platform
	   with no console to read. See Core/System/MobileLog.h. */
	MobileLog::Install();

	/* SDL decides the Android activity's orientation itself and overrides what
	   AndroidManifest.xml asked for - the first device run came up in portrait
	   at 1080x2400 despite a `sensorLandscape` manifest, because with no hint
	   set SDL requests SCREEN_ORIENTATION_FULL_USER. The manifest is still
	   right and still worth having (it is what the store reads); this is what
	   actually decides it at runtime.

	   Both landscapes, no portrait: the control scheme is two thumb sticks and
	   four buttons. */
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

	/* Then the assets: on Android and iOS this unpacks the packaged content
	   into writable storage and chdir()s there, which is what makes every bare
	   relative asset path in the engine resolve at all. A no-op on desktop.
	   See Core/System/MobileAssets.h. */
	if (!MobileAssets::PrepareAssetRoot())
	{
		fprintf(stderr, "[fatal] could not prepare the asset root\n");
		return 1;
	}

	/* Parsed before anything is constructed: --hidden and --size have to be
	   known by the time the window is created, and --map by the time the world
	   is loaded. --help returns false and exits. See LaunchOptions.h for why
	   this exists at all - it replaces editing ProjectSettings.vgps and
	   Settings.vgs to set up a measurement run. */
	if (!LaunchOptions::Get().Parse(argc, argv))
		return 0;

	VoxApp app;
	app.Run();

	return 0;
}
