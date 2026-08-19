#pragma once
#include <mutex>
#include <unordered_map>
#include "ReferenceObject.h"

/* Every one of these is shared by the whole process and reached from more than
   one thread. Chunk streaming phase 14 (ledger M9) is the demonstration: the
   asynchronous level switch deserializes the incoming world on a job thread,
   and deserializing an entity loads its model, its texture and its sound -
   through this map, while the outgoing world's chunk staging is doing exactly
   the same thing on the main thread. Two unsynchronised inserts into an
   unordered_map is a corrupted map; two threads both finding the *same*
   resource unloaded is two concurrent Loads into one object, which is where the
   fixture died with `double free or corruption (!prev)` inside VoxModel::Reset.

   The lock covers the load as well as the lookup (AddReferenceAndLoad), which
   is the point: "is it loaded, and if not load it" has to be one decision, or
   the second thread runs the whole of Load against a half-loaded object. It
   costs the other thread the duration of one resource load and nothing on a
   cache hit, which is the trade this tree can afford - a texture load already
   submits from job threads.

   Recursive because Release() calls back in: a reference object's Released
   event is subscribed here and lands in RemoveReference. */
template <typename T>
class ReferenceManager
{
static_assert(std::is_base_of<ReferenceObject, T>::value, "Type must derive from ReferenceObject");

public:
	~ReferenceManager() { ClearAll(); }

	T* AddReference(const std::string& ref);
	void RemoveReference(const std::string& ref);
	T* GetReference(const std::string& ref);
	void ClearAll();

	/* AddReference, plus the first-time load, as one critical section. Prepare
	   is called with the reference object only when it is not loaded yet, and is
	   where the caller does its SetContext/SetFileSystem/Load. */
	template <typename Prepare>
	T* AddReferenceAndLoad(const std::string& ref, Prepare&& prepare);

	void GetResourceFilePaths(std::vector<std::string>& ResourceFilePaths, std::string fileExtension = std::string());

private:
	std::unordered_map<std::string, T*> m_References;
	std::recursive_mutex m_Mutex;
};

template <typename T>
void ReferenceManager<T>::ClearAll()
{
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	typename std::unordered_map<std::string, T*>::iterator iter = m_References.begin();
	for (; iter != m_References.end(); ++iter)
	{
		if (iter->second)
		{
			delete iter->second;
			iter->second = nullptr;
		}
	}
	m_References.clear();
}

template<typename T>
inline void ReferenceManager<T>::GetResourceFilePaths(std::vector<std::string>& ResourceFilePaths, std::string fileExtension)
{
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	bool HasFileExtensionFilter = (fileExtension != std::string());
	if (HasFileExtensionFilter)
	{
		size_t founddot = fileExtension.find(".");
		if (founddot == std::string::npos || founddot != 0)
			fileExtension.insert(0, ".");
	}

	for (auto& it : m_References)
	{
		bool FileExtensionValid = true;

		if (HasFileExtensionFilter)
		{
			std::size_t found = it.first.find(".");

			if (found != std::string::npos)
			{
				if (it.first.substr(found, it.first.length() - found) != fileExtension)
					FileExtensionValid = false;
			}
			else
			{
				FileExtensionValid = false;
			}
		}

		if (FileExtensionValid)
			ResourceFilePaths.push_back(it.first);
	}

	std::sort(ResourceFilePaths.begin(), ResourceFilePaths.end());
}

template<typename T>
inline T* ReferenceManager<T>::AddReference(const std::string& ref)
{
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	typename std::unordered_map<std::string, T*>::iterator iter = m_References.find(ref);

	if (iter != m_References.end()) 
	{
		m_References[ref]->IncrementRef();
		return m_References[ref];
	}

	T* pRefObject = new T(ref);
	pRefObject->Released += Event<ReferenceObject*>::Subscriber([this](ReferenceObject* pRefObj) {
		RemoveReference(pRefObj->GetRefPath());
	}, this);
	m_References.emplace(ref, pRefObject);
	return pRefObject;
}

template <typename T>
template <typename Prepare>
inline T* ReferenceManager<T>::AddReferenceAndLoad(const std::string& ref, Prepare&& prepare)
{
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	T* pRefObject = AddReference(ref);

	if (!pRefObject->IsLoaded())
		prepare(pRefObject);

	return pRefObject;
}

template <typename T>
void ReferenceManager<T>::RemoveReference(const std::string& ref)
{
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	typename std::unordered_map<std::string, T*>::iterator iter = m_References.find(ref);

	if (iter != m_References.end())
	{
		iter->second->DecrementRef();
		if (iter->second->GetRefCount() <= 0) {
			delete iter->second;
			iter->second = nullptr;
			m_References.erase(iter);
		}
		return;
	}
}

template<typename T>
inline T* ReferenceManager<T>::GetReference(const std::string& ref)
{
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	typename std::unordered_map<std::string, T*>::iterator iter = m_References.find(ref);
	if (iter == m_References.end())
		return nullptr;
	return iter->second;
}