#include "VoxApp.h"

#include "Core/LaunchOptions.h"

/* SDL owns the entry point on every platform now, so the wWinMain variant and
   its <eventtoken.h> include are gone. */
int main(int argc, char* argv[])
{
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
