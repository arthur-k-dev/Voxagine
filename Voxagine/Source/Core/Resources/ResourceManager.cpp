#include "pch.h"
#include "ResourceManager.h"

#include "Core/Application.h"
#include "Core/Platform/Audio/AudioContext.h"
#include "Core/Platform/Rendering/RenderContextInc.h"

ResourceManager::ResourceManager(Application* pApp)
{
	m_pApp = pApp;
}

void ResourceManager::Unload()
{
	m_voxManager.ClearAll();
	m_textureManager.ClearAll();
	m_soundManager.ClearAll();
}

/* Every Load* below acquires-or-creates the reference *and* performs the
   first-time load inside the reference manager's lock. Splitting those two is
   what the asynchronous level switch crashed on - see ReferenceManager.h. */

TextureReference* ResourceManager::LoadTexture(const std::string& filePath)
{
	return m_textureManager.AddReferenceAndLoad(filePath,
		[this, &filePath](TextureReference* pTextureRef)
	{
		pTextureRef->SetContext(m_pApp->GetPlatform().GetRenderContext());
		pTextureRef->SetFileSystem(m_pApp->GetFileSystem());

		pTextureRef->Load(filePath);

		if (!pTextureRef->IsLoaded())
			LogFailedLoadResourceMessage("Texture", filePath);
	});
}

PlatformSoundReference* ResourceManager::LoadSound(const std::string& filePath)
{
	return m_soundManager.AddReferenceAndLoad(filePath,
		[this, &filePath](PlatformSoundReference* pSoundRef)
	{
		AudioContext* pAudioContext = m_pApp->GetPlatform().GetAudioContext();
		if (!pAudioContext)
			return;

		pSoundRef->SetContext(pAudioContext);
		pSoundRef->SetFileSystem(m_pApp->GetFileSystem());

		pSoundRef->Load(filePath);
		pSoundRef->m_fLength = pAudioContext->GetLength(pSoundRef);

		if (!pSoundRef->IsLoaded())
			LogFailedLoadResourceMessage("Sound", filePath);
	});
}

VoxModel* ResourceManager::LoadVox(const std::string& filePath)
{
	return m_voxManager.AddReferenceAndLoad(filePath,
		[this, &filePath](VoxModel* pModel)
	{
		pModel->SetContext(m_pApp->GetPlatform().GetRenderContext()->Get());
		pModel->SetFileSystem(m_pApp->GetFileSystem());

		pModel->Load(filePath);

		if (!pModel->IsLoaded())
			LogFailedLoadResourceMessage("VoxModel", filePath);
	});
}

std::vector<VoxModel*> ResourceManager::LoadVoxBatch(const std::string& filePath, const std::string& fileName)
{
	std::vector<VoxModel*> models = {};

	for (uint32_t i = 0; i < UINT32_MAX; ++i)
	{
		std::string file = filePath + fileName + "_" + std::to_string(i) + ".vox";
		bool bFailed = false;

		VoxModel* pModel = m_voxManager.AddReferenceAndLoad(file,
			[this, &file, &bFailed](VoxModel* pNewModel)
		{
			pNewModel->SetContext(m_pApp->GetPlatform().GetRenderContext()->Get());
			pNewModel->SetFileSystem(m_pApp->GetFileSystem());

			bFailed = !pNewModel->Load(file);
		});

		/* The batch ends at the first index that is not there, which is an
		   ordinary outcome rather than a failure: it is how the caller learns
		   how many frames an animation has. */
		if (bFailed)
		{
			m_voxManager.RemoveReference(file);
			break;
		}

		models.push_back(pModel);
	}

	return models;
}

VoxModel* ResourceManager::CreateHollowVox(const std::string& filePath)
{
	std::string newFilePath = filePath;
	auto len = filePath.find_last_of('/');

	if (len == std::string::npos)
	{
		len = 0;
	}
	else
	{
		len += 1;
	}

	newFilePath.insert(len, "hollow_");

	bool bFailed = false;

	VoxModel* pHollowModel = m_voxManager.AddReferenceAndLoad(newFilePath,
		[this, &filePath, &newFilePath, &bFailed](VoxModel* pNewModel)
	{
		pNewModel->SetContext(m_pApp->GetPlatform().GetRenderContext()->Get());
		pNewModel->SetFileSystem(m_pApp->GetFileSystem());

		pNewModel->Load(filePath);

		if (!pNewModel->IsLoaded())
		{
			bFailed = true;
			return;
		}

		pNewModel->MakeHollow(newFilePath);
	});

	return bFailed ? nullptr : pHollowModel;
}

void ResourceManager::GetResourceFilePaths(const std::string fileExtension, std::vector<std::string>& resourceFilePaths)
{
	if (fileExtension == "vox")
	{
		m_voxManager.GetResourceFilePaths(resourceFilePaths, fileExtension);
		return;
	}

	if (fileExtension == "anim.vox")
	{
		m_voxManager.GetResourceFilePaths(resourceFilePaths, fileExtension);
		return;
	}

	if (fileExtension == "ogg")
	{
		m_soundManager.GetResourceFilePaths(resourceFilePaths, fileExtension);
		return;
	}

	if (fileExtension == "png")
	{
		m_textureManager.GetResourceFilePaths(resourceFilePaths, fileExtension);
		return;
	}

	if(fileExtension == "wld")
	{
		resourceFilePaths = m_pApp->GetWorldManager().GetWorldFiles();
		return;
	}
}

void ResourceManager::LogFailedLoadResourceMessage(const std::string & resourceTypeName, const std::string & filePath)
{
	m_pApp->GetLoggingSystem().Log(LOGLEVEL_WARNING, "ResourceManager", "Failed to load " + resourceTypeName + " resource! Resource doesn't exists or not readable at location: " + filePath);
}
