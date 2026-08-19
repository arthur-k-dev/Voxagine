#include "nfd.h"

#import <AppKit/AppKit.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* The macOS file dialogs.
 *
 * This backend exists because the previous arrangement selected nfd_portal.cpp
 * here - the Linux one - and that is a trap rather than a gap. It *compiles* on
 * macOS: it needs only <poll.h>, popen and strdup, all of which exist. It then
 * looks for zenity or kdialog at runtime, finds neither on any Mac, prints a
 * line suggesting the user install them and returns NFD_CANCEL. So every File
 * menu item in the editor did nothing, and did it silently enough to look like
 * a UI bug rather than a missing backend.
 *
 * NSOpenPanel/NSSavePanel's -runModal spins its own event loop on the main
 * thread until the user is done. That is the same shape as the Win32 backend's
 * GetOpenFileName and unlike the portal one, which has to pump SDL by hand to
 * stop the compositor declaring the app hung - AppKit is already doing that
 * here, because it owns the loop.
 *
 * NFD's contract: on NFD_OKAY the caller owns the returned buffer and frees it
 * with free(), so paths are duplicated with strdup and not new[].
 */

namespace
{
	/* NFD filters look like "wld,prefab" or "Worlds:wld,prefab;Any:*". Only the
	   extension list matters; AppKit builds its own human-readable labels. */
	std::vector<std::string> ParseExtensions(const nfdchar_t* pFilterList)
	{
		std::vector<std::string> extensions;

		if (pFilterList == nullptr)
			return extensions;

		std::string current;

		for (const char* p = pFilterList; ; ++p)
		{
			const char c = *p;

			if (c == ',' || c == ';' || c == '\0')
			{
				/* Leading dots are tolerated because call sites are
				   inconsistent about them: Editor.cpp asks for "vox" in one
				   menu item and "." + the prefab extension in another. */
				if (!current.empty() && current.front() == '.')
					current.erase(current.begin());

				if (!current.empty() && current != "*")
					extensions.push_back(current);

				current.clear();

				if (c == '\0')
					break;

				continue;
			}

			/* Drop the human-readable half of "Label:ext". */
			if (c == ':')
			{
				current.clear();
				continue;
			}

			current += c;
		}

		return extensions;
	}

	NSArray<NSString*>* FileTypes(const nfdchar_t* pFilterList)
	{
		const std::vector<std::string> extensions = ParseExtensions(pFilterList);

		if (extensions.empty())
			return nil;

		NSMutableArray<NSString*>* types =
			[NSMutableArray arrayWithCapacity:extensions.size()];

		for (const std::string& extension : extensions)
			[types addObject:@(extension.c_str())];

		return types;
	}

	/* The engine's own file extensions - wld, vgs, prefab, vox - are not
	   registered content types on anyone's Mac, so there is no UTType to
	   describe them and -setAllowedContentTypes: cannot express the filter at
	   all. -setAllowedFileTypes: takes bare extension strings, which is
	   exactly what this has, and still works; it is deprecated rather than
	   removed, and the deprecation is silenced here rather than project-wide
	   so any *other* deprecated AppKit call still gets reported. */
	void SetAllowedExtensions(NSSavePanel* pPanel, const nfdchar_t* pFilterList)
	{
		NSArray<NSString*>* types = FileTypes(pFilterList);

		if (types == nil)
			return;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		[pPanel setAllowedFileTypes:types];
#pragma clang diagnostic pop
	}

	void SetDirectory(NSSavePanel* pPanel, const nfdchar_t* pDefaultPath)
	{
		if (pDefaultPath == nullptr || pDefaultPath[0] == '\0')
			return;

		[pPanel setDirectoryURL:[NSURL fileURLWithPath:@(pDefaultPath)]];
	}

	nfdresult_t Finish(NSURL* pURL, nfdchar_t** ppOutPath)
	{
		if (pURL == nil)
			return NFD_ERROR;

		const char* pPath = [[pURL path] fileSystemRepresentation];

		if (pPath == nullptr)
			return NFD_ERROR;

		*ppOutPath = strdup(pPath);

		return *ppOutPath != nullptr ? NFD_OKAY : NFD_ERROR;
	}
}

extern "C"
{
	nfdresult_t NFD_OpenDialog(const nfdchar_t* filterList, const nfdchar_t* defaultPath,
	                           nfdchar_t** outPath)
	{
		if (outPath == nullptr)
			return NFD_ERROR;

		*outPath = nullptr;

		/* The panel and everything it creates are autoreleased; without a pool
		   of our own they would accumulate until the frame loop's, and there
		   is no frame loop running while this is on screen. */
		@autoreleasepool
		{
			NSOpenPanel* pPanel = [NSOpenPanel openPanel];

			[pPanel setCanChooseFiles:YES];
			[pPanel setCanChooseDirectories:NO];
			[pPanel setAllowsMultipleSelection:NO];

			SetAllowedExtensions(pPanel, filterList);
			SetDirectory(pPanel, defaultPath);

			if ([pPanel runModal] != NSModalResponseOK)
				return NFD_CANCEL;

			return Finish([[pPanel URLs] firstObject], outPath);
		}
	}

	nfdresult_t NFD_SaveDialog(const nfdchar_t* filterList, const nfdchar_t* defaultPath,
	                           nfdchar_t** outPath)
	{
		if (outPath == nullptr)
			return NFD_ERROR;

		*outPath = nullptr;

		@autoreleasepool
		{
			NSSavePanel* pPanel = [NSSavePanel savePanel];

			/* Callers append the extension themselves when the returned path
			   lacks it, so the panel must not fight them over it. */
			[pPanel setExtensionHidden:NO];
			[pPanel setCanCreateDirectories:YES];

			SetAllowedExtensions(pPanel, filterList);
			SetDirectory(pPanel, defaultPath);

			if ([pPanel runModal] != NSModalResponseOK)
				return NFD_CANCEL;

			return Finish([pPanel URL], outPath);
		}
	}
}
