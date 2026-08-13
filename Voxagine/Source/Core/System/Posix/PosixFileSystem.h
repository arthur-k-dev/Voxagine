#pragma once

#include "Core/System/FileSystem.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

struct FileInfo
{
	FileInfo()
	{
		pFile = nullptr;
		FilePath = "";
		OpenFlags = static_cast<FSOpenFlags>(0);
	}
	FileInfo(FILE* _pFile, std::string _filePath, FSOpenFlags openFlags):
		pFile(_pFile),
		FilePath(_filePath),
		OpenFlags(openFlags) {}

	FILE* pFile;
	std::string FilePath;
	FSOpenFlags OpenFlags;
};

class PosixFileSystem : public FileSystem
{
public:
	PosixFileSystem(Application* pApp);

	virtual FH OpenFile(const char* pFilePath, FSOpenFlags openFlags) override;
	virtual FSResult CloseFile(FH fileHandle) override;
	virtual FSResult Read(FH fileHandle, void* pReadBuff, FSize elementSize, FSize length, FSize* pBytesRead = nullptr) override;
	virtual FSResult ReadAsync(FSAsyncReadInfo* pInfo) override;
	virtual FSResult Write(FH fileHandle, const void* pWriteBuff, FSize elementSize, FSize length) override;
	virtual FSize GetFileSize(FH fileHandle) override;
	virtual FSize FileTell(FH fileHandle) override;
	virtual FSResult FileSeek(FH fileHandle, FSize offset, FSSeekOrigin origin, FSize* pSeekPos = nullptr) override;

protected:
	virtual void Initialize() override;
	virtual void Deinitialize() override;

private:
	/* The whole map is reached from more than one thread and always was:
	   ReadAsync exists, chunk decoding runs on the IO worker, and chunk
	   streaming phase 14 added the loudest case - World::OpenWorldAsync reads a
	   level file on a job thread while the main thread is loading models for the
	   level it is still playing. Two unsynchronised inserts here is a corrupted
	   map, and a non-atomic ++ on the counter below hands two threads the *same*
	   handle, after which one of them closes the other's file.

	   The lock covers the map and not the I/O: OpenFile, CloseFile and the
	   lookups take it, and fread/fwrite/fseek run outside it against a FILE*
	   copied out under it. Holding it across the read would serialize every file
	   operation in the process against the slowest one, which is exactly what
	   the asynchronous load exists not to do - and glibc streams carry their own
	   per-stream lock, so two threads reading two files need nothing from us. */
	static std::atomic<uint32_t> m_FileHandleCtr;

	std::unordered_map<FH, FileInfo> m_FileMap;
	mutable std::mutex m_FileMapMutex;

	std::string FlagsToOpenMode(FSOpenFlags openFlags);
	bool IsHandleValid(FH fileHandle);

	/* The handle's file, copied out under the lock. False when the handle names
	   nothing, which is the only other answer any caller here wants. */
	bool TryGetFile(FH fileHandle, FileInfo& fileInfo) const;
};
