#include "pch.h"
#include "PlatformSoundReference.h"

#include "Core/Platform/Audio/AudioContext.h"

PlatformSoundReference::~PlatformSoundReference()
{
	Free();
}

bool PlatformSoundReference::Load(const std::string& filePath)
{
	if (m_pAudioContext == nullptr)
		return false;

	/* "_BGM" in the name is this codebase's music convention, and it is what
	   the backend reads to decide between decoding into memory and streaming
	   off disk. Keeping the test here rather than in the backend keeps the
	   convention in one place. */
	const bool bIs3D = filePath.find("_BGM") == std::string::npos;

	const bool bLoaded = m_pAudioContext->CreateSound(filePath, Sound, bIs3D);

	/* Optional sidecar naming the loop points, as "<start>;<end>;". */
	FH handle = m_pFileSystem->OpenFile((filePath + ".cfg").c_str(), FSOF_READ | FSOF_BINARY);

	if (handle)
	{
		FSize fileSize = m_pFileSystem->GetFileSize(handle);

		std::vector<char> buffer(fileSize);

		m_pFileSystem->Read(handle, buffer.data(), 1, fileSize);
		m_pFileSystem->CloseFile(handle);

		uint32_t index = 0;
		std::string value;

		for (uint32_t i = 0; i < fileSize; ++i)
		{
			const char c = buffer[i];

			if (c != ';')
			{
				value += c;
				continue;
			}

			/* A malformed sidecar used to throw out of a resource load. */
			try
			{
				const unsigned long ul = std::stoul(value, nullptr, 0);

				if (index == 0)
					m_uiLoopStart = ul;
				else
					m_uiLoopEnd = ul;
			}
			catch (const std::exception&)
			{
				break;
			}

			if (++index >= 2)
				break;

			value.clear();
		}
	}

	m_bIsLoaded = bLoaded;

	return bLoaded;
}

void PlatformSoundReference::Free()
{
	if (Sound != nullptr && m_pAudioContext != nullptr)
		m_pAudioContext->DestroySound(Sound);

	Sound = nullptr;
}
