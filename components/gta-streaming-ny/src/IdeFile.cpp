#include "StdInc.h"
#include "Hooking.h"
#include "IdeStore.h"
#include "Streaming.h"
#include "Pool.h"
#include "Entity.h"
#include <strsafe.h>

class CBaseModelInfo
{
public:
	char m_pad[56];
	uint32_t m_modelHash;
	uint32_t m_pad2;
	uint32_t m_refCount;

public:
	virtual ~CBaseModelInfo() = 0;

	virtual void Initialize() = 0;

	virtual void Release() = 0;
};

static CBaseModelInfo** g_modelInfoPtrs = (CBaseModelInfo**)0x15F73B0;

IdeFile::IdeFile(fwString fileName, int size, int idx)
	: m_required(false), m_loaded(false), m_ideIdx(idx), m_fileName(fileName), m_size(size), m_loading(false)
{
	m_boundingBox.fX1 = -9999.0f;
	m_boundingBox.fX2 = -9999.0f;
	m_boundingBox.fY1 = -9999.0f;
	m_boundingBox.fY2 = -9999.0f;

	m_bigFileName = va("stream:/%u", idx | 0x40000000);
}

void IdeFile::AddToBounds(uint16_t mi, const CRect& rect)
{
	if (m_modelIndices.find(mi) != m_modelIndices.end())
	{
		if (m_boundingBox.fX1 == -9999.0f || rect.fX1 < m_boundingBox.fX1)
		{
			m_boundingBox.fX1 = rect.fX1;
		}

		if (m_boundingBox.fX2 == -9999.0f || rect.fX2 > m_boundingBox.fX2)
		{
			m_boundingBox.fX2 = rect.fX2;
		}

		if (m_boundingBox.fY1 == -9999.0f || rect.fY1 > m_boundingBox.fY1)
		{
			m_boundingBox.fY1 = rect.fY1;
		}

		if (m_boundingBox.fY2 == -9999.0f || rect.fY2 < m_boundingBox.fY2)
		{
			m_boundingBox.fY2 = rect.fY2;
		}
	}
}

static void WRAPPER QueueRequest(int itemIdx, StreamRequestPage2* blocks, int numBlocks, int reqStart, void(*completionCB)(int userData, void* pageBuffer, uint32_t length, int),
								 int completionCBData, int a7, int a8) { EAXJMP(0x5B1080); }

void IdeFile::Request()
{
	if (m_loaded || m_loading)
	{
		return;
	}

	if (!CIdeStore::CanLoadAdditionalIde())
	{
		return;
	}

	trace(__FUNCTION__ " %s\n", m_fileName.c_str());

	m_loading = true;

	auto sw = GetStreamThread(0);

	if (g_nextStreamingItem == -1)
	{
		trace("loading ide %s got a neg1\n", m_fileName.c_str());

		return;
	}

	StreamRequest req = { 0 };
	req.completionCB = [] (int userData, void* buffer, uint32_t size, int)
	{
		CIdeStore::EnqueueRequestCompletion((IdeFile*)userData);
	};

	req.completionCBData = (int)this;

	m_streamItemIdx = g_nextStreamingItem;
	
	StreamingItem* item = &g_streamingItems[m_streamItemIdx];
	item->blockSize = m_size;
	item->device = (rage::fiDevice*)0xF21CA8;
	item->flags = 0;// m_size;

	// as the 'request free' function will attempt to free the filename
	item->fileName = (char*)rage::GetAllocator()->allocate(m_bigFileName.length() + 1, 16, 0);
	strcpy(item->fileName, m_bigFileName.c_str());

	g_nextStreamingItem = item->streamCounter;
	item->streamCounter = 0;
	/*InterlockedIncrement(&item->streamCounter);

	InterlockedIncrement((DWORD*)0x184A25C);
	
	m_textBuffer = new char[m_size + (16384 - (m_size % 16384))];

	req.itemIdx = item->handle;
	req.pages[0].length = m_size;
	req.pages[0].buffer = m_textBuffer;
	req.reqStart = 0;
	req.reqLength = 1; // 1 page, clearly
	
	sw->QueueNativeRequest(req);*/

	CIdeStore::EnqueueRequestBegin(this);

	m_textBuffer = new char[m_size + (16384 - (m_size % 16384))];

	StreamRequestPage2 pages[4] = { 0 };

	pages[0].length = m_size;
	pages[0].buffer = m_textBuffer;

	QueueRequest(item->handle, pages, 1, 0, req.completionCB, req.completionCBData, 0, 0);
}

static std::set<uint32_t> g_modelInfosToRelease;

struct CDataStore
{
	uint32_t size;
	uint32_t allocated;
	void* data;
};

struct ModelInfoToHash_t
{
	uint32_t hash;
	int modelIndex;
};

atArray<ModelInfoToHash_t>& g_modelInfosToHash = *(atArray<ModelInfoToHash_t>*)0xF2C240;

static uint32_t WRAPPER NatHash(const char* str) { EAXJMP(0x7BDBF0); }

static IdeFile* g_curIdeFile;

template<CDataStore* dataStorePtr, int elemSize>
void* AllocateModelInfo(const char* name)
{
	CDataStore& dataStore = *dataStorePtr;

	// scan for free modelinfo storage
	bool found = false;
	int i;

	uint32_t nameHash = NatHash(name);

	if (nameHash == 0x338F6062)
	{
		printf("");
	}

	for (i = 0; i < dataStore.size; i++)
	{
		CBaseModelInfo* modelInfo = (CBaseModelInfo*)((char*)dataStore.data + (elemSize * i));

		if (modelInfo->m_modelHash == nameHash || modelInfo->m_modelHash == -2)
		{
			found = true;
			break;
		}
	}

	if (!found)
	{
		i = dataStore.allocated++;
	}

	// get the ptr to this modelinfo
	CBaseModelInfo* thisInfo = (CBaseModelInfo*)((char*)dataStore.data + (elemSize * i));

	thisInfo->Initialize();
	thisInfo->m_modelHash = nameHash;

	// find a free idx
	found = false;

	for (i = 0; i < 31000; i++)
	{
		if (g_modelInfoPtrs[i] == thisInfo)
		{
			found = true;
			break;
		}
	}

	if (!found)
	{
		for (i = 0; i < 31000; i++)
		{
			if (g_modelInfoPtrs[i] == nullptr)
			{
				break;
			}
		}
	}

	if (i == 31000)
	{
		FatalError("help what is modelinfo");
	}

	if (g_curIdeFile)
	{
		g_curIdeFile->AddModelIndex(i);
	}

	// set the idx
	int idx = i;
	g_modelInfoPtrs[i] = thisInfo;

	// officially, model idx count; however for us 'fun'
	if (*(uint32_t*)0x15F73A4 <= i)
	{
		*(uint32_t*)0x15F73A4 = i + 1;
	}

	// set some bool ('is model-to-hash array sorted')
	hook::put<uint8_t>(0xF2C23C, 0);

	// model hash to info mapping

	// first find if this model hash already has an entry
	for (i = 0; i < g_modelInfosToHash.GetCount(); i++)
	{
		auto& entry = g_modelInfosToHash.Get(i);

		if (entry.hash == thisInfo->m_modelHash)
		{
			entry.modelIndex = idx;
			
			return thisInfo;
		}
	}

	ModelInfoToHash_t newEntry;
	newEntry.hash = thisInfo->m_modelHash;
	newEntry.modelIndex = idx;

	g_modelInfosToHash.Set(g_modelInfosToHash.GetCount(), newEntry);

	return thisInfo;
}

void WRAPPER LoadObjects(const char* fileName) { EAXJMP(0x8D67E0); }

void IdeFile::DoLoad()
{
	if (m_loaded || !m_loading)
	{
		return;
	}

	trace(__FUNCTION__ " %s\n", m_fileName.c_str());

	g_curIdeFile = this;

	m_loaded = true;
	m_loading = false;

	// as old model indices won't apply anymore
	m_modelIndices.clear();

	// load the scene?
	LoadObjects(va("memory:$%p,%d,%d:%s", m_textBuffer, m_size, 0, 0));

	// sort the model-to-hash array
	((void(*)())0x98AB90)();

	auto imgManager = CImgManager::GetInstance();

	int startIndex = 0;

	// create streaming objects
	for (auto mi : m_modelIndices)
	{
		auto info = g_modelInfoPtrs[mi];
		auto& ideEntryIt = CIdeStore::ms_drawables.find(info->m_modelHash);

		if (ideEntryIt == CIdeStore::ms_drawables.end())
		{
			continue;
		}

		auto ideEntry = ideEntryIt->second;

		char fileName[128];
		StringCbCopyA(fileName, sizeof(fileName), ideEntry.filename.c_str());
		fileName[strlen(fileName) - 3] = 'w'; // wdr instead of zdr

		startIndex = imgManager->registerIMGFile(fileName, 0, ideEntry.size, 0xFE, 65535, ideEntry.rscVersion);

		imgManager->fileDatas[startIndex].realSize = ideEntry.rscFlags;

		if (ideEntry.rscFlags & 0x40000000)
		{
			imgManager->fileDatas[startIndex].flags |= 0x2000;
		}

		CSM_CreateStreamingFile(startIndex, ideEntry);
	}

	g_curIdeFile = nullptr;

	delete[] m_textBuffer;
}

struct CStreamingInfo
{
	char pad[8];
	uint8_t flags;
	char pad2[3];
	uint16_t owners;
	uint16_t pad3;
	char pad4[8];

	void RemoveFromList();
};

void WRAPPER CStreamingInfo::RemoveFromList() { EAXJMP(0xBCBB70); }

struct CStreamingStuff
{
	CStreamingInfo* items;
};

#include "StreamingTypes.h"

static CStreamingStuff& stuff = *(CStreamingStuff*)0xF21C60;

static void ReleaseModelInfo(int modelIdx)
{
	uint32_t modelHash = g_modelInfoPtrs[modelIdx]->m_modelHash;

	g_modelInfoPtrs[modelIdx]->Release();
	g_modelInfoPtrs[modelIdx]->m_modelHash = -2;

	g_modelInfoPtrs[modelIdx] = nullptr;

	int streamIdx = streamingTypes.types[*(int*)0x15F73A0].startIndex + modelIdx;

	stuff.items[streamIdx].owners = 0;
	stuff.items[streamIdx].flags &= ~3;

	stuff.items[streamIdx].RemoveFromList();

	for (int i = 0; i < g_modelInfosToHash.GetCount(); i++)
	{
		auto& entry = g_modelInfosToHash.Get(i);

		if (entry.hash == modelHash)
		{
			//entry.hash = 0;
			//break;
			g_modelInfosToHash.Remove(i);
			break;
		}
	}

	// remove any buildings
	/*auto pool = CPools::GetBuildingPool();

	for (int i = 0; i < pool->GetCount(); i++)
	{
		auto item = pool->GetAt<CEntity>(i);

		if (item != nullptr)
		{
			if (item->m_nModelIndex == modelIdx)
			{
				item->Remove();
				break;
			}
		}
	}*/
}

void IdeFile::Delete()
{
	if (!m_loaded)
	{
		return;
	}

	trace(__FUNCTION__ " %s\n", m_fileName.c_str());
	
	m_loading = false;
	m_loaded = false;

	for (auto& idx : m_modelIndices)
	{
		auto mi = g_modelInfoPtrs[idx];

		if (!mi)
		{
			continue;
		}

		if (mi->m_refCount > 0)
		{
			g_modelInfosToRelease.insert(idx);
		}
		else
		{
			//trace("Destructing model info 0x%08x instantly\n", mi->m_modelHash);

			ReleaseModelInfo(idx);
		}
	}
}

static int FindModelInfoIdxWithHash(uint32_t modelHash)
{
	for (int i = 0; i < 31000; i++)
	{
		CBaseModelInfo* modelInfo = g_modelInfoPtrs[i];

		if (modelInfo->m_modelHash == modelHash)
		{
			return i;
		}
	}

	return -1;
}

static void __fastcall ModelInfoReleaseTail(char* modelInfo)
{
	// ref count
	uint32_t refCount = *(uint32_t*)(modelInfo + 68);
	uint32_t modelHash = *(uint32_t*)(modelInfo + 60);

	if (refCount == 0)
	{
		//trace("Released last ref on model info 0x%08x from %p.\n", modelHash, _ReturnAddress());

		if (g_modelInfosToRelease.find(modelHash) != g_modelInfosToRelease.end())
		{
			//trace("Destructing model info 0x%08x w/ delay\n", modelHash);

			ReleaseModelInfo(FindModelInfoIdxWithHash(modelHash));
		}
	}
}

static int GetMIRefCount(int mIdx)
{
	auto mi = g_modelInfoPtrs[mIdx];

	if (!mi)
	{
		return 0;
	}

	return mi->m_refCount;
}

static int GetMIParents(int mIdx, int* a2)
{
	auto mi = g_modelInfoPtrs[mIdx];

	if (!mi)
	{
		return 0;
	}

	return ((int(*)(int, int*))0x989160)(mIdx, a2);
}

static bool MIOnLoad(int mIdx, intptr_t a2, intptr_t a3)
{
	auto mi = g_modelInfoPtrs[mIdx];

	if (!mi)
	{
		return false;
	}

	return ((bool(*)(int, intptr_t, intptr_t))0x989760)(mIdx, a2, a3);
}

CDataStore& g_baseModelStore = *(CDataStore*)0xF2C11C;
CDataStore& g_timeModelStore = *(CDataStore*)0xF2C134;

static HookFunction hookFunction([] ()
{
	// CBaseModelInfo release
	hook::jump(0x98E474, ModelInfoReleaseTail);

	// modelinfo allocators
	hook::jump(0x98AC60, AllocateModelInfo<&g_baseModelStore, 96>);
	hook::jump(0x98AD40, AllocateModelInfo<&g_timeModelStore, 112>);

	// modelinfo 'get refcount'
	hook::put(0x98B00F, GetMIRefCount);

	// modelinfo 'get parent infos'
	hook::put(0x98B00A, GetMIParents);

	// modelinfo 'on load'
	hook::put(0x98B032, MIOnLoad);
});