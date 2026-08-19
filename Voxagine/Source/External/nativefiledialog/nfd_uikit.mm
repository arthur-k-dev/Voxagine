#include "nfd.h"

#import <UIKit/UIKit.h>

#include <cstring>
#include <string>
#include <vector>

/* The iOS file dialogs.
 *
 * **Not a UIDocumentPicker, and that is the interesting decision here.** The
 * picker is the obvious answer and it is the wrong control for this caller.
 * Every use of these functions in Editor.cpp does the same thing with the
 * result:
 *
 *     if (m_FileBrowser.OpenFile(filePath, "vox"))
 *         if (m_FileBrowser.AbsoluteToRelative(filePath, outPath,
 *                 m_ProjectSettings.GetContentFolderPath()))
 *             ... otherwise "Can't load file because it's not located in the
 *                 Content folder"
 *
 * A path outside the project's content tree is rejected, because the engine
 * addresses every asset by a path relative to it. A document picker's entire
 * purpose is to reach files elsewhere - iCloud, Files, another app's shared
 * container - and a security-scoped URL to one of those is exactly what this
 * editor cannot use. It would present a system-quality browser that could only
 * usefully return results from the one directory it is worst at reaching.
 *
 * So this browses the project itself. The working directory is the prepared
 * asset root by the time any of this runs (Core/System/MobileAssets.h), so
 * "the project" is simply "." and Content/ is right there in it.
 *
 * The other half of the problem is that NFD is synchronous - it returns a path
 * or a cancel - and UIKit is not. The dialog therefore runs a nested run loop
 * until the user is done, which is the same thing SDL's own iOS backend does
 * inside SDL_PumpEvents, and the same shape as the Win32 backend's
 * GetOpenFileName. The engine's frame loop is a plain while loop on the main
 * thread rather than a display-link callback, so parking it here re-enters
 * nothing; no frames are submitted while the dialog is up, and the GPU is
 * idle rather than starved.
 *
 * NFD's contract: on NFD_OKAY the caller owns the returned buffer and frees it
 * with free(), so paths are duplicated with strdup and not new[].
 */

namespace
{
	/* NFD filters look like "wld,prefab" or "Worlds:wld,prefab;Any:*". */
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
				/* Call sites are inconsistent about the leading dot -
				   Editor.cpp asks for "vox" in one menu item and "." + the
				   prefab extension in another. */
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

	NSArray<NSString*>* ExtensionList(const nfdchar_t* pFilterList)
	{
		const std::vector<std::string> extensions = ParseExtensions(pFilterList);
		NSMutableArray<NSString*>* list = [NSMutableArray array];

		for (const std::string& extension : extensions)
			[list addObject:[@(extension.c_str()) lowercaseString]];

		return list;
	}

	UIViewController* PresentingController()
	{
		UIWindow* pKeyWindow = nil;

		for (UIScene* pScene in [[UIApplication sharedApplication] connectedScenes])
		{
			if (![pScene isKindOfClass:[UIWindowScene class]])
				continue;

			for (UIWindow* pWindow in [static_cast<UIWindowScene*>(pScene) windows])
			{
				if ([pWindow isKeyWindow])
				{
					pKeyWindow = pWindow;
					break;
				}
			}

			if (pKeyWindow != nil)
				break;
		}

		if (pKeyWindow == nil)
			return nil;

		/* SDL's own view controller, which owns the Metal layer the renderer
		   draws into. Presenting on top of it is what puts the dialog over the
		   frozen last frame rather than over a black screen. */
		UIViewController* pRoot = [pKeyWindow rootViewController];

		while ([pRoot presentedViewController] != nil)
			pRoot = [pRoot presentedViewController];

		return pRoot;
	}
}

/* Shared by every controller pushed onto the dialog's navigation stack, so a
   selection three directories deep still reaches the waiting caller. */
@interface NFDDialogSession : NSObject
@property(nonatomic, assign) BOOL finished;
@property(nonatomic, strong) NSString* pickedPath;
@property(nonatomic, strong) NSArray<NSString*>* extensions;
@property(nonatomic, assign) BOOL saveMode;
@end

@implementation NFDDialogSession
@end

@interface NFDBrowserController : UITableViewController
@property(nonatomic, strong) NFDDialogSession* session;
@property(nonatomic, strong) NSString* directory;
@property(nonatomic, strong) NSArray<NSString*>* directories;
@property(nonatomic, strong) NSArray<NSString*>* files;
@end

@implementation NFDBrowserController

- (void)viewDidLoad
{
	[super viewDidLoad];

	self.title = [self.directory lastPathComponent];

	self.navigationItem.leftBarButtonItem =
		[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
		                                              target:self
		                                              action:@selector(cancel)];

	if (self.session.saveMode)
	{
		self.navigationItem.rightBarButtonItem =
			[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemSave
			                                              target:self
			                                              action:@selector(promptForName)];
	}

	[self reload];
}

- (void)reload
{
	NSFileManager* pManager = [NSFileManager defaultManager];
	NSArray<NSString*>* entries =
		[pManager contentsOfDirectoryAtPath:self.directory error:nil];

	NSMutableArray<NSString*>* directories = [NSMutableArray array];
	NSMutableArray<NSString*>* files = [NSMutableArray array];

	for (NSString* pEntry in [entries sortedArrayUsingSelector:@selector(localizedStandardCompare:)])
	{
		/* The stamp file MobileAssets writes, and anything else dot-prefixed.
		   None of it is project content. */
		if ([pEntry hasPrefix:@"."])
			continue;

		NSString* pFull = [self.directory stringByAppendingPathComponent:pEntry];
		BOOL bIsDirectory = NO;

		if (![pManager fileExistsAtPath:pFull isDirectory:&bIsDirectory])
			continue;

		if (bIsDirectory)
		{
			[directories addObject:pEntry];
			continue;
		}

		/* An empty filter means "anything", which is what NFD's own
		   convention is for a null filter list. */
		if ([self.session.extensions count] > 0 &&
		    ![self.session.extensions containsObject:[[pEntry pathExtension] lowercaseString]])
			continue;

		[files addObject:pEntry];
	}

	self.directories = directories;
	self.files = files;

	[self.tableView reloadData];
}

- (void)cancel
{
	/* finished without a path is a cancel; the caller distinguishes them by
	   pickedPath being nil. */
	self.session.finished = YES;
	[self dismiss];
}

- (void)dismiss
{
	[[[self navigationController] presentingViewController]
		dismissViewControllerAnimated:YES completion:nil];
}

- (void)finishWithPath:(NSString*)path
{
	self.session.pickedPath = path;
	self.session.finished = YES;
	[self dismiss];
}

- (void)promptForName
{
	UIAlertController* pAlert =
		[UIAlertController alertControllerWithTitle:@"Save As"
		                                    message:self.directory
		                             preferredStyle:UIAlertControllerStyleAlert];

	[pAlert addTextFieldWithConfigurationHandler:^(UITextField* pField) {
		pField.placeholder = @"File name";
		pField.autocorrectionType = UITextAutocorrectionTypeNo;
		pField.autocapitalizationType = UITextAutocapitalizationTypeNone;
	}];

	[pAlert addAction:[UIAlertAction actionWithTitle:@"Cancel"
	                                           style:UIAlertActionStyleCancel
	                                         handler:nil]];

	__weak NFDBrowserController* pWeakSelf = self;

	[pAlert addAction:[UIAlertAction actionWithTitle:@"Save"
	                                           style:UIAlertActionStyleDefault
	                                         handler:^(UIAlertAction*) {
		NSString* pName = [[[pAlert textFields] firstObject] text];

		if ([pName length] == 0)
			return;

		/* The caller appends the extension itself when the returned path
		   lacks it, exactly as it does with the desktop dialogs, so nothing
		   is forced on the name here. */
		[pWeakSelf finishWithPath:
			[pWeakSelf.directory stringByAppendingPathComponent:pName]];
	}]];

	[self presentViewController:pAlert animated:YES completion:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView
{
	return 2;
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section
{
	if (section == 0)
		return [self.directories count] > 0 ? @"Folders" : nil;

	return [self.files count] > 0 ? @"Files" : nil;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section
{
	return section == 0 ? static_cast<NSInteger>([self.directories count])
	                    : static_cast<NSInteger>([self.files count]);
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath
{
	UITableViewCell* pCell =
		[tableView dequeueReusableCellWithIdentifier:@"nfd"];

	if (pCell == nil)
	{
		pCell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
		                               reuseIdentifier:@"nfd"];
	}

	const BOOL bIsDirectory = indexPath.section == 0;
	NSArray<NSString*>* entries = bIsDirectory ? self.directories : self.files;

	pCell.textLabel.text = entries[static_cast<NSUInteger>(indexPath.row)];
	pCell.accessoryType = bIsDirectory ? UITableViewCellAccessoryDisclosureIndicator
	                                   : UITableViewCellAccessoryNone;

	return pCell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];

	if (indexPath.section == 0)
	{
		NFDBrowserController* pChild =
			[[NFDBrowserController alloc] initWithStyle:UITableViewStyleGrouped];

		pChild.session = self.session;
		pChild.directory = [self.directory stringByAppendingPathComponent:
			self.directories[static_cast<NSUInteger>(indexPath.row)]];

		[[self navigationController] pushViewController:pChild animated:YES];
		return;
	}

	NSString* pPath = [self.directory stringByAppendingPathComponent:
		self.files[static_cast<NSUInteger>(indexPath.row)]];

	/* In save mode a tap on an existing file is "overwrite this one", which is
	   how every other save dialog behaves; the caller's own overwrite handling
	   takes it from there. */
	[self finishWithPath:pPath];
}

@end

namespace
{
	nfdresult_t RunDialog(const nfdchar_t* pFilterList, const nfdchar_t* pDefaultPath,
	                      nfdchar_t** ppOutPath, bool bSave)
	{
		if (ppOutPath == nullptr)
			return NFD_ERROR;

		*ppOutPath = nullptr;

		@autoreleasepool
		{
			UIViewController* pPresenter = PresentingController();

			if (pPresenter == nil)
			{
				fprintf(stderr, "[nfd] no window to present a file dialog on\n");
				return NFD_ERROR;
			}

			/* The prepared asset root - MobileAssets chdir'd here before the
			   first file was read, so it is both the project root and the only
			   writable copy of it. */
			NSString* pRoot = (pDefaultPath != nullptr && pDefaultPath[0] != '\0')
				? @(pDefaultPath)
				: [[NSFileManager defaultManager] currentDirectoryPath];

			NFDDialogSession* pSession = [[NFDDialogSession alloc] init];
			pSession.extensions = ExtensionList(pFilterList);
			pSession.saveMode = bSave ? YES : NO;

			NFDBrowserController* pBrowser =
				[[NFDBrowserController alloc] initWithStyle:UITableViewStyleGrouped];

			pBrowser.session = pSession;
			pBrowser.directory = pRoot;

			UINavigationController* pNavigation =
				[[UINavigationController alloc] initWithRootViewController:pBrowser];

			/* Not full screen: the editor behind it stays visible, which is
			   the difference between "a dialog is open" and "the app froze". */
			pNavigation.modalPresentationStyle = UIModalPresentationFormSheet;

			[pPresenter presentViewController:pNavigation animated:YES completion:nil];

			/* The nested run loop. UIKit needs the main thread to lay out,
			   animate and deliver touches, and the engine's frame loop is not
			   going to give it back on its own - see the file comment. */
			while (!pSession.finished)
			{
				@autoreleasepool
				{
					[[NSRunLoop currentRunLoop]
						runMode:NSDefaultRunLoopMode
						beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
				}
			}

			if (pSession.pickedPath == nil)
				return NFD_CANCEL;

			const char* pPath = [pSession.pickedPath fileSystemRepresentation];

			if (pPath == nullptr)
				return NFD_ERROR;

			*ppOutPath = strdup(pPath);

			return *ppOutPath != nullptr ? NFD_OKAY : NFD_ERROR;
		}
	}
}

extern "C"
{
	nfdresult_t NFD_OpenDialog(const nfdchar_t* filterList, const nfdchar_t* defaultPath,
	                           nfdchar_t** outPath)
	{
		return RunDialog(filterList, defaultPath, outPath, false);
	}

	nfdresult_t NFD_SaveDialog(const nfdchar_t* filterList, const nfdchar_t* defaultPath,
	                           nfdchar_t** outPath)
	{
		return RunDialog(filterList, defaultPath, outPath, true);
	}
}
