#include "Core/System/MobileAssets.h"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(VOXAGINE_MOBILE)
#include <chrono>
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
#if defined(VOXAGINE_MOBILE)
	const char* const k_pStampFile = ".voxagine-assets";

	/* The engine's whole content tree lives under these. Restricting the copy
	   to them keeps everything Gradle happens to sweep into assets/ - and on
	   Android that includes its own bookkeeping directories - out of the way. */
	const char* const k_pAssetRoots[] = { "Content", "Engine" };

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
#if !defined(VOXAGINE_MOBILE)

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

	/* The private, writable, per-app directory. Survives updates, is wiped on
	   uninstall, and needs no permission on either platform. */
	std::string storage;

#if defined(VOXAGINE_ANDROID)
	/* Owned by SDL. */
	if (const char* pStorage = SDL_GetAndroidInternalStoragePath())
		storage = pStorage;
#else
	/* Owned by the caller, unlike every other SDL path getter. */
	if (char* pStorage = SDL_GetPrefPath("Voxagine", "BitBuster"))
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
		std::filesystem::remove_all(root, ec);
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

		/* The loose files beside them - Settings.vgs and the project settings -
		   are not under a directory, so they are copied by name. */
		for (const char* pLooseFile : { "Settings.vgs", "ProjectSettings.vgps" })
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
		/* iOS: the bundle is a real directory, so this is a plain recursive
		   copy. It exists at all because the bundle is read-only and the game
		   writes Settings.vgs and PlayerPrefs through the same relative paths
		   it reads assets through. */
		const char* pBundle = SDL_GetBasePath();

		if (pBundle == nullptr)
		{
			fprintf(stderr, "[assets] no bundle path\n");
			return false;
		}

		std::error_code copyError;
		std::filesystem::copy(std::filesystem::path(pBundle), root,
			std::filesystem::copy_options::recursive |
			std::filesystem::copy_options::overwrite_existing,
			copyError);

		if (copyError)
		{
			fprintf(stderr, "[assets] copy from bundle failed: %s\n", copyError.message().c_str());
			return false;
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
