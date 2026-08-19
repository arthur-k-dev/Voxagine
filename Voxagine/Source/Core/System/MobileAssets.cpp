#include "Core/System/MobileAssets.h"

#include <cstdio>
#include <cstring>
#include <string>

/* "Packaged" means the assets ship inside an application bundle or archive
   rather than sitting beside the working directory. That is the condition this
   file actually cares about, and it is not the same as "mobile":

     Android  - packaged. Assets are zip entries in the APK.
     iOS      - packaged. Assets are real files in a read-only bundle.
     macOS    - packaged *sometimes*. The editor is a double-clickable .app
                (CMakeLists.txt), and a bundle launched from Finder starts with
                the working directory set to "/", so relative asset paths
                resolve against the root of the disk. The same build run from a
                terminal out of Game/ is not packaged and must not be touched.
                Only the runtime can tell those apart; see FindBundleRoot.
     Linux,
     Windows  - never packaged. Launched from Game/, as they always were.

   So macOS compiles the machinery in and decides per launch, which is why this
   is __APPLE__ rather than a build-time bundle flag: one binary, both ways of
   starting it. */
#if defined(VOXAGINE_MOBILE) || defined(__APPLE__)
#define VOXAGINE_PACKAGED_ASSETS 1
#endif

#if defined(VOXAGINE_PACKAGED_ASSETS)
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <unistd.h>

#include <SDL3/SDL.h>
#endif

#if defined(VOXAGINE_ANDROID)
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#endif

/* Bumped by the packaging build (Gradle passes the app's versionName). A
   mismatch against the stamp on disk re-extracts, which is what makes an app
   update actually ship its new content instead of running last version's. */
#ifndef VOXAGINE_ASSET_VERSION
#define VOXAGINE_ASSET_VERSION "dev"
#endif

namespace
{
#if defined(VOXAGINE_PACKAGED_ASSETS)
	const char* const k_pStampFile = ".voxagine-assets";

	/* The engine's whole content tree lives under these. Restricting the copy
	   to them keeps everything Gradle happens to sweep into assets/ - and on
	   Android that includes its own bookkeeping directories - out of the way. */
	const char* const k_pAssetRoots[] = { "Content", "Engine" };

	/* Configuration that sits beside them rather than inside them, so it has
	   to be named. Must match voxagine_bundle_assets() in CMakeLists.txt: a
	   file shipped there but missing here is never extracted, and one named
	   here but not shipped is simply absent at runtime.

	   UserSettings.vguser is editor-only and holds the startup world. A game
	   build neither ships nor reads it, and asking for it there would just
	   fail the copy harmlessly - but the guard keeps the two lists honest. */
	const char* const k_pLooseFiles[] = {
		"Settings.vgs",
		"ProjectSettings.vgps",
#if defined(EDITOR)
		"UserSettings.vguser",
#endif
	};

	/* Small files, thousands of them, so this is deliberately not large: the
	   cost here is syscalls, not bandwidth. */
	const size_t k_uiCopyBufferBytes = 64 * 1024;

	bool ReadStamp(const std::filesystem::path& root, std::string& out)
	{
		FILE* pFile = fopen((root / k_pStampFile).string().c_str(), "rb");

		if (pFile == nullptr)
			return false;

		char buffer[256] = {};
		const size_t uiRead = fread(buffer, 1, sizeof(buffer) - 1, pFile);
		fclose(pFile);

		out.assign(buffer, uiRead);
		return true;
	}

	bool WriteStamp(const std::filesystem::path& root)
	{
		FILE* pFile = fopen((root / k_pStampFile).string().c_str(), "wb");

		if (pFile == nullptr)
			return false;

		const char* pVersion = VOXAGINE_ASSET_VERSION;
		fwrite(pVersion, 1, strlen(pVersion), pFile);
		fclose(pFile);

		return true;
	}
#endif

#if defined(VOXAGINE_PACKAGED_ASSETS) && !defined(VOXAGINE_ANDROID)
	/* How a refresh treats a file that is already in the writable root.
	 *
	 * A game's root holds nothing but a copy of the bundle, so the bundle
	 * always wins. An editor's root is also where its output lives, and a file
	 * it saved yesterday is newer than the one shipped in a bundle rebuilt
	 * today only in the cases where the shipped one genuinely changed - so
	 * newer-wins refreshes the assets that moved and leaves the work alone. */
#if defined(EDITOR)
	constexpr std::filesystem::copy_options k_RefreshPolicy =
		std::filesystem::copy_options::update_existing;
#else
	constexpr std::filesystem::copy_options k_RefreshPolicy =
		std::filesystem::copy_options::overwrite_existing;
#endif

	/* Where the shipped asset tree is, or nothing if this launch is not from a
	   bundle at all.

	   SDL_GetBasePath answers a different question on each platform and both
	   answers are the ones wanted here: on iOS it is the bundle root, on macOS
	   it is Contents/Resources. CMakeLists.txt's voxagine_bundle_assets() is
	   handed the matching destination for each, so Content/ and Engine/ are
	   directly under whatever this returns.

	   On macOS it is also the *test*. An unbundled build - the ordinary
	   `cmake --build` output run from a terminal - gets the executable's own
	   directory back, which does not end in Contents/Resources, and that is
	   what distinguishes "double-clicked .app with a working directory of /"
	   from "run from Game/ the way it always was". Getting this wrong in the
	   permissive direction would chdir a developer's terminal run away from
	   their project. */
	bool FindBundleRoot(std::filesystem::path& out)
	{
		const char* pBase = SDL_GetBasePath();

		if (pBase == nullptr)
			return false;

		std::filesystem::path base(pBase);

#if !defined(VOXAGINE_IOS)
		/* SDL returns this with a trailing separator, which makes filename()
		   the empty string rather than the last directory - so the component
		   test below has to be done on the parent. Dropping the separator
		   first is clearer than writing every comparison one level up. */
		std::filesystem::path normalized = base.lexically_normal();

		if (!normalized.has_filename())
			normalized = normalized.parent_path();

		if (normalized.filename() != "Resources" ||
		    normalized.parent_path().filename() != "Contents")
			return false;
#endif

		std::error_code ec;

		if (!std::filesystem::exists(base / k_pAssetRoots[0], ec))
		{
			/* Reached only from inside a bundle, so this is a packaging fault
			   rather than a normal launch, and silence would present as the
			   app starting to a black screen. */
			fprintf(stderr, "[assets] %s looks like a bundle but has no %s directory\n",
				base.string().c_str(), k_pAssetRoots[0]);
			return false;
		}

		out = std::move(base);
		return true;
	}
#endif

#if defined(VOXAGINE_ANDROID)
	/* Android's asset manager has one gap that decides the shape of all of
	   this: the C API (AAssetManager_openDir) does not report subdirectories,
	   only files, so a recursive walk is impossible through it. The Java
	   AssetManager.list() *does* return both. So enumeration goes through JNI
	   and the actual reads go through the C API, which is the cheaper half. */
	class AndroidAssetSource
	{
	public:
		bool Open()
		{
			m_pEnv = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
			m_Activity = static_cast<jobject>(SDL_GetAndroidActivity());

			if (m_pEnv == nullptr || m_Activity == nullptr)
			{
				fprintf(stderr, "[assets] no JNI environment; is this running under SDLActivity?\n");
				return false;
			}

			jclass activityClass = m_pEnv->GetObjectClass(m_Activity);
			jmethodID getAssets = m_pEnv->GetMethodID(activityClass, "getAssets",
				"()Landroid/content/res/AssetManager;");
			m_pEnv->DeleteLocalRef(activityClass);

			if (getAssets == nullptr)
				return false;

			jobject localManager = m_pEnv->CallObjectMethod(m_Activity, getAssets);

			if (localManager == nullptr)
				return false;

			/* A local reference is only valid until this native call returns to
			   Java, and the AAssetManager* borrows from it - so it has to be
			   promoted, and released again in Close(). */
			m_AssetManagerObject = m_pEnv->NewGlobalRef(localManager);
			m_pEnv->DeleteLocalRef(localManager);

			m_pAssetManager = AAssetManager_fromJava(m_pEnv, m_AssetManagerObject);

			if (m_pAssetManager == nullptr)
				return false;

			jclass managerClass = m_pEnv->GetObjectClass(m_AssetManagerObject);
			m_ListMethod = m_pEnv->GetMethodID(managerClass, "list",
				"(Ljava/lang/String;)[Ljava/lang/String;");
			m_pEnv->DeleteLocalRef(managerClass);

			return m_ListMethod != nullptr;
		}

		void Close()
		{
			if (m_pEnv == nullptr)
				return;

			if (m_AssetManagerObject != nullptr)
				m_pEnv->DeleteGlobalRef(m_AssetManagerObject);

			/* SDL_GetAndroidActivity hands out a *local* reference and says so
			   in its header - it is the caller's to release. */
			if (m_Activity != nullptr)
				m_pEnv->DeleteLocalRef(m_Activity);

			m_AssetManagerObject = nullptr;
			m_Activity = nullptr;
			m_pAssetManager = nullptr;
		}

		bool List(const std::string& path, std::vector<std::string>& out)
		{
			out.clear();

			jstring jPath = m_pEnv->NewStringUTF(path.c_str());
			jobjectArray entries = static_cast<jobjectArray>(
				m_pEnv->CallObjectMethod(m_AssetManagerObject, m_ListMethod, jPath));

			m_pEnv->DeleteLocalRef(jPath);

			if (m_pEnv->ExceptionCheck())
			{
				m_pEnv->ExceptionClear();
				return false;
			}

			if (entries == nullptr)
				return false;

			const jsize iCount = m_pEnv->GetArrayLength(entries);

			for (jsize i = 0; i < iCount; ++i)
			{
				jstring entry = static_cast<jstring>(m_pEnv->GetObjectArrayElement(entries, i));
				const char* pName = m_pEnv->GetStringUTFChars(entry, nullptr);

				if (pName != nullptr)
					out.push_back(pName);

				m_pEnv->ReleaseStringUTFChars(entry, pName);
				m_pEnv->DeleteLocalRef(entry);
			}

			m_pEnv->DeleteLocalRef(entries);
			return true;
		}

		/* Copies one asset out. Returns the byte count, or -1. */
		long long CopyTo(const std::string& assetPath, const std::filesystem::path& destination)
		{
			AAsset* pAsset = AAssetManager_open(m_pAssetManager, assetPath.c_str(), AASSET_MODE_STREAMING);

			if (pAsset == nullptr)
				return -1;

			FILE* pOut = fopen(destination.string().c_str(), "wb");

			if (pOut == nullptr)
			{
				AAsset_close(pAsset);
				return -1;
			}

			std::vector<char> buffer(k_uiCopyBufferBytes);
			long long iTotal = 0;

			for (;;)
			{
				const int iRead = AAsset_read(pAsset, buffer.data(), buffer.size());

				if (iRead < 0)
				{
					iTotal = -1;
					break;
				}

				if (iRead == 0)
					break;

				if (fwrite(buffer.data(), 1, static_cast<size_t>(iRead), pOut) != static_cast<size_t>(iRead))
				{
					iTotal = -1;
					break;
				}

				iTotal += iRead;
			}

			fclose(pOut);
			AAsset_close(pAsset);

			return iTotal;
		}

	private:
		JNIEnv* m_pEnv = nullptr;
		jobject m_Activity = nullptr;
		jobject m_AssetManagerObject = nullptr;
		AAssetManager* m_pAssetManager = nullptr;
		jmethodID m_ListMethod = nullptr;
	};

	/* Asset entries carry no type, so each one is *tried* as a file first and
	   treated as a directory only if that fails. The obvious alternative -
	   asking list() whether it has children - costs a JNI round trip per entry
	   over a tree of several thousand files; this costs one C-level open, and
	   the open is the work that has to happen for a file anyway.

	   The ambiguous case is an empty directory, which reads as a file that
	   would not open. aapt drops those, so it does not arise. */
	bool ExtractTree(AndroidAssetSource& source, const std::string& assetPath,
	                 const std::filesystem::path& destination,
	                 uint32_t& uiFiles, long long& iBytes)
	{
		std::vector<std::string> entries;

		if (!source.List(assetPath, entries))
			return false;

		if (entries.empty())
			return true;

		std::error_code ec;
		std::filesystem::create_directories(destination, ec);

		for (const std::string& entry : entries)
		{
			const std::string childAsset = assetPath.empty() ? entry : assetPath + "/" + entry;
			const std::filesystem::path childPath = destination / entry;

			const long long iWritten = source.CopyTo(childAsset, childPath);

			if (iWritten >= 0)
			{
				++uiFiles;
				iBytes += iWritten;
				continue;
			}

			if (!ExtractTree(source, childAsset, childPath, uiFiles, iBytes))
			{
				fprintf(stderr, "[assets] failed to extract %s\n", childAsset.c_str());
				return false;
			}
		}

		return true;
	}
#endif
}

namespace MobileAssets
{
#if !defined(VOXAGINE_PACKAGED_ASSETS)

bool PrepareAssetRoot()
{
	/* Desktop resolves assets against the working directory the game was
	   launched from, which is Game/. Nothing to do, and deliberately no
	   silent chdir - a run from the wrong directory should fail loudly the way
	   it always has. */
	return true;
}

#else

bool PrepareAssetRoot()
{
	const auto start = std::chrono::steady_clock::now();

#if !defined(VOXAGINE_ANDROID)
	/* Where the shipped tree is. Empty means this launch is not from a bundle,
	   which on macOS is the ordinary case: a build run from Game/ resolves its
	   assets against the working directory exactly as it did before this file
	   grew a desktop branch. */
	std::filesystem::path bundle;

	if (!FindBundleRoot(bundle))
	{
#if defined(VOXAGINE_IOS)
		fprintf(stderr, "[assets] no bundle path\n");
		return false;
#else
		return true;
#endif
	}

	/* An escape hatch, and one the macOS editor in particular needs.
	 *
	 * Everything below copies the bundle's assets into a private container and
	 * works there, which is right for a shipped app and wrong for the machine
	 * this project is developed on: worlds saved from Voxagine.app would land
	 * in ~/Library/Application Support and never reach the repository the
	 * assets came from. Pointing this at the checkout's Game/ directory makes
	 * the editor edit the real project.
	 *
	 * It is read before the container is prepared and skips it entirely, so it
	 * costs neither the copy nor the disk. */
	if (const char* pOverride = std::getenv("VOXAGINE_ASSET_ROOT"))
	{
		if (chdir(pOverride) != 0)
		{
			fprintf(stderr, "[assets] VOXAGINE_ASSET_ROOT is set to %s, which could not be entered\n",
				pOverride);
			return false;
		}

		printf("[assets] asset root is %s (VOXAGINE_ASSET_ROOT)\n", pOverride);
		return true;
	}
#endif

	/* The private, writable, per-app directory. Survives updates, is wiped on
	   uninstall, and needs no permission on any of these platforms. */
	std::string storage;

#if defined(VOXAGINE_ANDROID)
	/* Owned by SDL. */
	if (const char* pStorage = SDL_GetAndroidInternalStoragePath())
		storage = pStorage;
#else
	/* Owned by the caller, unlike every other SDL path getter.
	 *
	 * The editor gets its own directory. On iOS the container is per-bundle-ID
	 * and they could not collide anyway, but on macOS both apps run as the
	 * same user against the same Application Support tree, and the editor
	 * *writes* into its asset root - a shared one would mean saving a world in
	 * the editor changed what the game loaded, silently and only for whoever
	 * ran both. */
#if defined(EDITOR)
	if (char* pStorage = SDL_GetPrefPath("Voxagine", "VoxagineEditor"))
#else
	if (char* pStorage = SDL_GetPrefPath("Voxagine", "BitBuster"))
#endif
	{
		storage = pStorage;
		SDL_free(pStorage);
	}
#endif

	if (storage.empty())
	{
		fprintf(stderr, "[assets] no writable storage path available\n");
		return false;
	}

	const std::filesystem::path root = std::filesystem::path(storage) / "assets";

	std::string stamp;
	const bool bCurrent = ReadStamp(root, stamp) && stamp == VOXAGINE_ASSET_VERSION;

	if (!bCurrent)
	{
		printf("[assets] extracting content for version %s\n", VOXAGINE_ASSET_VERSION);

		std::error_code ec;

#if !defined(EDITOR)
		/* Wiped first so a stale tree cannot leave a file the new version
		   deleted lying around to be loaded.

		   Not in an editor build: this root is where the editor's own work
		   lives - worlds it saved, prefabs it exported, models it imported -
		   and none of that came out of the bundle, so a version bump would
		   delete it with no warning and no copy anywhere else. The refresh
		   below is update_existing instead, which brings over shipped assets
		   that actually changed and leaves everything else alone. */
		std::filesystem::remove_all(root, ec);
#endif

		std::filesystem::create_directories(root, ec);

		if (ec)
		{
			fprintf(stderr, "[assets] could not create %s: %s\n",
				root.string().c_str(), ec.message().c_str());
			return false;
		}

		uint32_t uiFiles = 0;
		long long iBytes = 0;

#if defined(VOXAGINE_ANDROID)
		AndroidAssetSource source;

		if (!source.Open())
		{
			/* Open can fail part-way through, and it is holding a JNI local
			   reference by then. Close is written to tolerate that. */
			source.Close();
			return false;
		}

		bool bOk = true;

		for (const char* pAssetRoot : k_pAssetRoots)
			bOk = bOk && ExtractTree(source, pAssetRoot, root / pAssetRoot, uiFiles, iBytes);

		/* The loose files beside them are not under a directory, so they are
		   copied by name. */
		for (const char* pLooseFile : k_pLooseFiles)
		{
			const long long iWritten = source.CopyTo(pLooseFile, root / pLooseFile);

			if (iWritten >= 0)
			{
				++uiFiles;
				iBytes += iWritten;
			}
		}

		source.Close();

		if (!bOk)
			return false;
#else
		/* iOS and a macOS .app: the bundle is a real directory, so the tree can
		   be reached with ordinary filesystem calls. This step exists at all
		   because the bundle is read-only and the engine writes Settings.vgs
		   and PlayerPrefs through the same relative paths it reads assets
		   through.

		   Restricted to k_pAssetRoots for the same reason Android is, and the
		   cost of not doing so is higher here: the bundle root also holds the
		   executable, its _CodeSignature, and the desktop project's leftovers
		   (Source/, the Optick DLLs, the .rc files). Copying those would roughly
		   double the install's footprint on device for nothing, and would put a
		   second copy of the executable somewhere App Review does not permit
		   one. */

		/* Symlinked in a game build, copied in an editor build, and the
		   difference is what the two do with Content/.
		 *
		 * A game only ever reads it, so a symlink is strictly better: the
		 * bundle is a real readable directory that stays mounted for the life
		 * of the app, and duplicating ~94 MB of read-only assets into the
		 * container bought nothing but a slower first launch and double the
		 * install footprint. (Android cannot do this at all - its assets live
		 * inside the APK and have no path - which is why only this branch has
		 * the choice.)
		 *
		 * The editor writes there constantly: saving a world, exporting a
		 * prefab, VoxModel::Save. Through a symlink every one of those lands
		 * inside the application bundle, which fails outright once the app is
		 * signed and quietly corrupts its own signature when it does not. So
		 * the editor pays for a real copy and owns what it writes.
		 *
		 * Either way the files the engine writes through relative paths -
		 * Settings.vgs, PlayerPrefs - are real files in the writable root. */
		for (const char* pAssetRoot : k_pAssetRoots)
		{
#if !defined(EDITOR)
			std::error_code linkError;
			std::filesystem::create_directory_symlink(
				bundle / pAssetRoot, root / pAssetRoot, linkError);

			if (!linkError)
			{
				++uiFiles;
				continue;
			}

			/* A sandbox that refuses the symlink is recoverable - it just costs
			   what this used to cost - so fall back rather than refusing to
			   start. */
			fprintf(stderr, "[assets] symlink of %s failed (%s); copying instead\n",
				pAssetRoot, linkError.message().c_str());
#endif

			std::error_code copyError;
			std::filesystem::copy(bundle / pAssetRoot, root / pAssetRoot,
				std::filesystem::copy_options::recursive | k_RefreshPolicy,
				copyError);

			if (copyError)
			{
				fprintf(stderr, "[assets] copy of %s from the bundle failed: %s\n",
					pAssetRoot, copyError.message().c_str());
				return false;
			}
		}

		for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
		{
			if (entry.is_regular_file(ec))
			{
				++uiFiles;
				iBytes += static_cast<long long>(entry.file_size(ec));
			}
		}
#endif

		if (!WriteStamp(root))
			fprintf(stderr, "[assets] could not write the stamp; this will re-extract next launch\n");

		const auto finish = std::chrono::steady_clock::now();
		const double dSeconds = std::chrono::duration<double>(finish - start).count();

		printf("[assets] extracted %u files, %.1f MiB, in %.2f s\n",
			uiFiles, static_cast<double>(iBytes) / (1024.0 * 1024.0), dSeconds);
	}

#if !defined(VOXAGINE_ANDROID)
	/* Seeded on every launch, outside the extraction, and both halves of that
	 * are deliberate.
	 *
	 * copy_options::none means "fail if it already exists", so a file the user
	 * now owns is never overwritten - these are settings they changed and, in
	 * an editor build, the world it last had open. Unlike Content/ there is no
	 * upstream version worth preferring, so the error is the expected outcome
	 * and is ignored.
	 *
	 * Every launch rather than only when the stamp is stale, because otherwise
	 * a loose file *added* in a new build never arrives: the stamp is "dev" in
	 * an ordinary build and never changes, so the extraction is skipped and
	 * the new file is simply missing forever. That is exactly what happened
	 * when UserSettings.vguser was added - the editor started with no world
	 * and the bundle looked correct. Three copy_file calls that fail
	 * immediately is not a cost worth optimising away. */
	for (const char* pLooseFile : k_pLooseFiles)
	{
		std::error_code seedError;
		std::filesystem::copy_file(bundle / pLooseFile, root / pLooseFile,
			std::filesystem::copy_options::none, seedError);
	}
#endif

	if (chdir(root.string().c_str()) != 0)
	{
		fprintf(stderr, "[assets] could not enter %s\n", root.string().c_str());
		return false;
	}

	printf("[assets] asset root is %s\n", root.string().c_str());

	return true;
}

#endif
}
