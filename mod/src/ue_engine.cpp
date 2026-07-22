#include "ue_engine.h"
#include <Windows.h>
#include <cstring>
#include <unordered_map>
#include <mutex>

#ifdef GetClassName
#undef GetClassName
#endif

extern void Log(const char* fmt, ...);

namespace UEEngine {

static uintptr_t g_moduleBase = 0;
static uintptr_t g_moduleEnd = 0;
static char g_initStatus[256] = "Not initialized";

// Raw pointers discovered at runtime (not using fixed struct offsets)
struct ObjArrayInfo {
    void**   ChunkArray;  // FUObjectItem** — array of chunk pointers
    int32_t  NumElements;
    int32_t  MaxElements;
    int32_t  NumChunks;
    int32_t* LiveNumElementsPtr; // pointer to the LIVE NumElements in game memory
};
static ObjArrayInfo g_objArray = {};

using FNameToStringFn = void(__fastcall*)(const UE::FName*, UE::FString*);
static FNameToStringFn g_fnameToString = nullptr;

using FNameFNameFn = void(__fastcall*)(UE::FName* outName, const wchar_t* str, int32_t findType);
static FNameFNameFn g_fnameFromString = nullptr;

struct FMalloc { void** VTable; };
static FMalloc** g_GMalloc = nullptr;

static UE::ProcessEventFn g_processEvent = nullptr;
static int g_processEventVtableIdx = -1;

static std::unordered_map<uint64_t, std::wstring> g_nameCache;
static std::mutex g_nameCacheMutex;

uintptr_t GetModuleBase() { return g_moduleBase; }
const char* GetInitStatus() { return g_initStatus; }
bool HasProcessEvent() { return g_processEvent != nullptr; }

int GetObjectCount() { return g_objArray.NumElements; }

void FreeFString(UE::FString& str) {
    if (!str.Data || !g_GMalloc || !*g_GMalloc) return;
    auto* m = *g_GMalloc;
    using FreeFn = void(__fastcall*)(void*, void*);
    auto fn = (FreeFn)m->VTable[5];
    fn(m, str.Data);
    str.Data = nullptr;
    str.Num = 0;
    str.Max = 0;
}

// FMalloc vtable (confirmed by Free=[5]): Malloc=[1], Realloc=[3], Free=[5].
void* GameMalloc(size_t size) {
    if (!g_GMalloc || !*g_GMalloc) return nullptr;
    auto* m = *g_GMalloc;
    using MallocFn = void*(__fastcall*)(void*, size_t, uint32_t);
    return ((MallocFn)m->VTable[1])(m, size, 8);
}
void* GameRealloc(void* original, size_t size) {
    if (!g_GMalloc || !*g_GMalloc) return nullptr;
    auto* m = *g_GMalloc;
    using ReallocFn = void*(__fastcall*)(void*, void*, size_t, uint32_t);
    return ((ReallocFn)m->VTable[3])(m, original, size, 8);
}

// Safely read memory, returns false if access violation
static bool SafeRead(const void* addr, void* buf, size_t len) {
    __try {
        memcpy(buf, addr, len);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadU64(const void* addr, uint64_t* out) {
    return SafeRead(addr, out, 8);
}

static bool SafeReadI32(const void* addr, int32_t* out) {
    return SafeRead(addr, out, 4);
}

// Scan GUObjectArray region for the FChunkedFixedUObjectArray substructure.
// We look for a pattern: [ptr, ptr, int32 MaxElements, int32 NumElements, int32 MaxChunks, int32 NumChunks]
// where NumElements is in a sane range and MaxElements >= NumElements.
static bool FindObjectArray(uintptr_t guobjectArrayAddr) {
    Log("[UE] Scanning GUObjectArray at 0x%llX for object array...\n", guobjectArrayAddr);

    // Scan the first 0x100 bytes of the GUObjectArray looking for the chunked array
    for (int offset = 0; offset < 0x100; offset += 8) {
        uintptr_t base = guobjectArrayAddr + offset;

        uint64_t ptrVal = 0;
        int32_t val1 = 0, val2 = 0, val3 = 0, val4 = 0;

        if (!SafeReadU64((void*)base, &ptrVal)) continue;
        if (!SafeReadI32((void*)(base + 0x10), &val1)) continue;  // MaxElements
        if (!SafeReadI32((void*)(base + 0x14), &val2)) continue;  // NumElements
        if (!SafeReadI32((void*)(base + 0x18), &val3)) continue;  // MaxChunks
        if (!SafeReadI32((void*)(base + 0x1C), &val4)) continue;  // NumChunks

        // Heuristic: NumElements should be 1000-5000000, MaxElements >= NumElements,
        // NumChunks should be 1-100, MaxChunks >= NumChunks,
        // and the pointer should be in a valid heap range
        if (val2 > 1000 && val2 < 5000000 &&
            val1 >= val2 &&
            val4 >= 1 && val4 <= 200 &&
            val3 >= val4 &&
            ptrVal > 0x10000 && ptrVal < 0x7FFFFFFFFFFF) {

            // Validate: try to read the first chunk pointer
            uint64_t firstChunk = 0;
            if (!SafeReadU64((void*)ptrVal, &firstChunk)) continue;
            if (firstChunk < 0x10000 || firstChunk > 0x7FFFFFFFFFFF) continue;

            // Validate: try to read an object from the first chunk
            uint64_t firstObj = 0;
            if (!SafeReadU64((void*)firstChunk, &firstObj)) continue;

            g_objArray.ChunkArray = (void**)ptrVal;
            g_objArray.NumElements = val2;
            g_objArray.MaxElements = val1;
            g_objArray.NumChunks = val4;
            g_objArray.LiveNumElementsPtr = (int32_t*)(base + 0x14);

            Log("[UE] Found object array at offset 0x%X: NumElements=%d MaxElements=%d NumChunks=%d ChunkArray=0x%llX\n",
                offset, val2, val1, val4, ptrVal);
            Log("[UE] First chunk=0x%llX, first object ptr=0x%llX\n", firstChunk, firstObj);
            return true;
        }
    }

    Log("[UE] Failed to find object array in GUObjectArray\n");
    return false;
}

// 0x15C84B0 = UObject::ProcessEvent, verified in IDA: FUNC_Net check with
// GetFunctionCallspace(slot 77)/CallRemoteFunction(slot 78), alloca'd parm frame,
// FFrame construction + UFunction::Invoke, out-parm copyback.
// (The old value 0x5A93500/slot 103 was UNetDriver::ProcessRemoteFunction.)
static const uintptr_t KNOWN_PROCESSEVENT_RVA = 0x15C84B0;
static const int KNOWN_PROCESSEVENT_VTABLE_IDX = 76;

static bool FindProcessEvent() {
    // Use known address from IDA analysis (confirmed via vtable + RPC dispatch)
    uintptr_t knownAddr = g_moduleBase + KNOWN_PROCESSEVENT_RVA;
    uint8_t probe = 0;
    if (SafeRead((void*)knownAddr, &probe, 1) && probe != 0x00 && probe != 0xCC) {
        g_processEvent = (UE::ProcessEventFn)knownAddr;
        g_processEventVtableIdx = KNOWN_PROCESSEVENT_VTABLE_IDX;
        Log("[PE] ProcessEvent at known RVA 0x%llX (vtable[%d])\n",
            KNOWN_PROCESSEVENT_RVA, KNOWN_PROCESSEVENT_VTABLE_IDX);
        return true;
    }
    Log("[PE] Known ProcessEvent RVA invalid, falling back to scan...\n");

    uintptr_t gnativesAddr = g_moduleBase + UE::RVA::GNatives;
    Log("[PE] Scanning for GNatives refs (target=0x%llX)...\n", gnativesAddr);

    uint8_t* textStart = (uint8_t*)g_moduleBase + 0x1000;
    uint8_t* textEnd   = (uint8_t*)g_moduleEnd;
    uintptr_t callFunctionAddr = 0;

    for (uint8_t* p = textStart; p < textEnd - 7; p++) {
        if ((p[0] & 0xFB) != 0x48) continue;
        if (p[1] != 0x8D) continue;
        uint8_t modrm = p[2];
        if ((modrm & 0xC7) != 0x05) continue;

        int32_t disp = *(int32_t*)(p + 3);
        uintptr_t target = (uintptr_t)(p + 7) + disp;

        if (target == gnativesAddr) {
            uintptr_t funcStart = 0;
            for (uint8_t* scan = p; scan > textStart && scan > p - 0x2000; scan--) {
                if (scan[-1] == 0xCC || scan[-1] == 0x90 || scan[-1] == 0xC3) {
                    funcStart = (uintptr_t)scan;
                    break;
                }
            }
            if (funcStart) {
                Log("[PE] Found GNatives ref in function at RVA 0x%llX\n", funcStart - g_moduleBase);
                callFunctionAddr = funcStart;
                break;
            }
        }
    }

    if (!callFunctionAddr) {
        Log("[PE] No GNatives reference found\n");
        return false;
    }

    uintptr_t processEventAddr = 0;
    for (uint8_t* p = textStart; p < textEnd - 5; p++) {
        if (p[0] != 0xE8) continue;
        int32_t disp = *(int32_t*)(p + 1);
        uintptr_t target = (uintptr_t)(p + 5) + disp;

        if (target == callFunctionAddr) {
            for (uint8_t* scan = p; scan > textStart && scan > p - 0x1000; scan--) {
                if (scan[-1] == 0xCC || scan[-1] == 0x90 || scan[-1] == 0xC3) {
                    processEventAddr = (uintptr_t)scan;
                    break;
                }
            }
            if (processEventAddr) {
                Log("[PE] ProcessEvent candidate at RVA 0x%llX\n", processEventAddr - g_moduleBase);
                break;
            }
        }
    }

    if (!processEventAddr) {
        Log("[PE] Could not find ProcessEvent\n");
        return false;
    }

    // Find vtable slot by checking a UClass object's vtable
    UE::UObject* anyClass = nullptr;
    ForEachObject([&](UE::UObject* obj) -> bool {
        std::wstring cn = GetClassName(obj);
        if (cn == L"Class") {
            anyClass = obj;
            return false;
        }
        return true;
    });

    if (anyClass && anyClass->VTable) {
        for (int i = 50; i < 120; i++) {
            uint64_t slot = 0;
            if (SafeReadU64(&anyClass->VTable[i], &slot) && slot == processEventAddr) {
                g_processEventVtableIdx = i;
                g_processEvent = (UE::ProcessEventFn)processEventAddr;
                Log("[PE] ProcessEvent = vtable[%d]\n", i);
                return true;
            }
        }
    }

    g_processEvent = (UE::ProcessEventFn)processEventAddr;
    Log("[PE] ProcessEvent found (direct call, no vtable match)\n");
    return true;
}

bool Init() {
    g_moduleBase = (uintptr_t)GetModuleHandleW(nullptr);
    if (!g_moduleBase) {
        sprintf_s(g_initStatus, "GetModuleHandle failed");
        return false;
    }

    Log("[UE] Module base = 0x%llX\n", g_moduleBase);

    auto* dos = (IMAGE_DOS_HEADER*)g_moduleBase;
    auto* nt  = (IMAGE_NT_HEADERS*)(g_moduleBase + dos->e_lfanew);
    g_moduleEnd = g_moduleBase + nt->OptionalHeader.SizeOfImage;
    Log("[UE] Module end = 0x%llX (size=0x%llX)\n", g_moduleEnd, g_moduleEnd - g_moduleBase);

    g_fnameToString = (FNameToStringFn)(g_moduleBase + UE::RVA::FNameToString);
    g_fnameFromString = (FNameFNameFn)(g_moduleBase + UE::RVA::FNameFName);
    g_GMalloc = (FMalloc**)(g_moduleBase + UE::RVA::GMalloc);
    Log("[UE] FNameToString = 0x%llX\n", (uintptr_t)g_fnameToString);
    Log("[UE] FNameFromString = 0x%llX\n", (uintptr_t)g_fnameFromString);
    Log("[UE] GMalloc ptr = 0x%llX\n", (uintptr_t)g_GMalloc);

    uintptr_t guobjectArrayAddr = g_moduleBase + UE::RVA::GUObjectArray;

    if (!FindObjectArray(guobjectArrayAddr)) {
        sprintf_s(g_initStatus, "Failed to find object array");
        return false;
    }

    // Test FName::ToString on first object
    UE::UObject* firstObj = nullptr;
    ForEachObject([&](UE::UObject* obj) -> bool {
        firstObj = obj;
        return false;
    });

    if (firstObj) {
        Log("[UE] Testing FName::ToString on first object...\n");
        std::wstring testName = FNameToString(firstObj->NamePrivate);
        Log("[UE] First object: name=%ls class=%ls\n",
            testName.c_str(), GetClassName(firstObj).c_str());
    } else {
        Log("[UE] Warning: no objects found in array\n");
    }

    Log("[UE] Searching for ProcessEvent...\n");
    FindProcessEvent();

    sprintf_s(g_initStatus, "OK (%d objs, PE=%s[%d])",
        g_objArray.NumElements,
        g_processEvent ? "yes" : "no",
        g_processEventVtableIdx);
    Log("[UE] Init complete: %s\n", g_initStatus);
    return true;
}

static bool SafeCallFNameToString(FNameToStringFn fn, const UE::FName* name, UE::FString* out) {
    __try {
        fn(name, out);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring FNameToString(UE::FName name) {
    if (!g_fnameToString)
        return L"<no_fname_fn>";

    uint64_t key = ((uint64_t)name.Number << 32) | (uint32_t)name.ComparisonIndex;

    {
        std::lock_guard lock(g_nameCacheMutex);
        auto it = g_nameCache.find(key);
        if (it != g_nameCache.end())
            return it->second;
    }

    UE::FString result = {};
    if (!SafeCallFNameToString(g_fnameToString, &name, &result))
        return L"<crash>";

    std::wstring str = result.ToWString();
    FreeFString(result);

    {
        std::lock_guard lock(g_nameCacheMutex);
        g_nameCache[key] = str;
    }

    return str;
}

UE::FName FNameFromString(const wchar_t* str) {
    UE::FName result = {0, 0};
    if (!g_fnameFromString || !str) return result;

    __try {
        g_fnameFromString(&result, str, 1); // FNAME_Add=1 (register if not present)
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result = {0, 0};
    }
    return result;
}

UE::FName FNameFindExisting(const wchar_t* str) {
    UE::FName result = {0, 0};
    if (!g_fnameFromString || !str) return result;

    __try {
        // FNAME_Find=0: returns NAME_None if the name was never registered.
        // Use this for asset names — FNAME_Add would fabricate a valid-looking
        // FName for a nonexistent asset, which injects as a blank pickup.
        g_fnameFromString(&result, str, 0);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result = {0, 0};
    }
    return result;
}

void ForEachObject(const std::function<bool(UE::UObject*)>& callback) {
    if (!g_objArray.ChunkArray || g_objArray.NumElements <= 0) return;

    // Read LIVE element count (objects are added during gameplay)
    int32_t liveCount = g_objArray.NumElements;
    if (g_objArray.LiveNumElementsPtr) {
        SafeReadI32(g_objArray.LiveNumElementsPtr, &liveCount);
        if (liveCount <= 0 || liveCount > g_objArray.MaxElements)
            liveCount = g_objArray.NumElements;
    }

    for (int i = 0; i < liveCount; i++) {
        int chunkIdx = i / UE::OBJECTS_PER_CHUNK;
        int itemIdx  = i % UE::OBJECTS_PER_CHUNK;

        void* chunkPtr = nullptr;
        if (!SafeReadU64(&g_objArray.ChunkArray[chunkIdx], (uint64_t*)&chunkPtr)) continue;
        if (!chunkPtr) continue;

        // FUObjectItem is 24 bytes: [UObject* (8), Flags (4), ClusterRoot (4), Serial (4), Pad (4)]
        uint8_t* itemBase = (uint8_t*)chunkPtr + (size_t)itemIdx * 24;
        UE::UObject* obj = nullptr;
        if (!SafeReadU64(itemBase, (uint64_t*)&obj)) continue;
        if (!obj) continue;

        // Basic validation: check object pointer looks sane
        if ((uintptr_t)obj < 0x10000 || (uintptr_t)obj > 0x7FFFFFFFFFFF) continue;

        // Validate VTable pointer
        uint64_t vtablePtr = 0;
        if (!SafeReadU64(obj, &vtablePtr)) continue;
        if (vtablePtr < 0x10000 || vtablePtr > 0x7FFFFFFFFFFF) continue;

        if (!callback(obj))
            return;
    }
}

std::wstring GetObjectName(UE::UObject* obj) {
    if (!obj) return L"None";
    UE::FName name = {};
    if (!SafeRead(&obj->NamePrivate, &name, sizeof(name))) return L"<bad_ptr>";
    return FNameToString(name);
}

std::wstring GetClassName(UE::UObject* obj) {
    if (!obj) return L"None";
    void* classPtr = nullptr;
    if (!SafeReadU64(&obj->ClassPrivate, (uint64_t*)&classPtr)) return L"<bad_ptr>";
    if (!classPtr) return L"None";

    UE::FName name = {};
    if (!SafeRead(&((UE::UObject*)classPtr)->NamePrivate, &name, sizeof(name))) return L"<bad_ptr>";
    return FNameToString(name);
}

std::wstring GetFullName(UE::UObject* obj) {
    if (!obj) return L"None";
    std::wstring name = GetClassName(obj) + L" ";

    std::vector<std::wstring> path;
    UE::UObject* current = obj;
    int depth = 0;
    while (current && depth < 20) {
        path.push_back(GetObjectName(current));
        void* outer = nullptr;
        if (!SafeReadU64(&current->OuterPrivate, (uint64_t*)&outer)) break;
        current = (UE::UObject*)outer;
        depth++;
    }

    for (int i = (int)path.size() - 1; i >= 0; i--) {
        name += path[i];
        if (i > 0) name += L".";
    }

    return name;
}

UE::UObject* FindObject(const wchar_t* className, const wchar_t* objectName) {
    UE::UObject* result = nullptr;
    ForEachObject([&](UE::UObject* obj) -> bool {
        if (className) {
            std::wstring cn = GetClassName(obj);
            if (cn != className) return true;
        }
        if (objectName) {
            std::wstring on = GetObjectName(obj);
            if (on != objectName) return true;
        }
        result = obj;
        return false;
    });
    return result;
}

std::vector<UE::UObject*> FindObjectsOfClass(const wchar_t* className) {
    std::vector<UE::UObject*> results;
    ForEachObject([&](UE::UObject* obj) -> bool {
        std::wstring cn = GetClassName(obj);
        if (cn == className)
            results.push_back(obj);
        return true;
    });
    return results;
}

UE::UObject* FindFunction(UE::UObject* classObj, const wchar_t* funcName) {
    if (!classObj) return nullptr;

    for (int childrenOffset : {0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70}) {
        uint8_t* classBytes = (uint8_t*)classObj;
        uint64_t childAddr = 0;
        if (!SafeReadU64(classBytes + childrenOffset, &childAddr)) continue;
        if (childAddr < 0x10000 || childAddr > 0x7FFFFFFFFFFF) continue;

        UE::UField* child = (UE::UField*)childAddr;
        if (IsBadReadPtr(child, sizeof(UE::UField))) continue;

        for (UE::UField* f = child; f; ) {
            if (IsBadReadPtr(f, sizeof(UE::UField))) break;

            UE::FName fname = {};
            if (!SafeRead(&f->NamePrivate, &fname, sizeof(fname))) break;

            std::wstring name = FNameToString(fname);
            if (name == funcName)
                return (UE::UObject*)f;

            uint64_t nextAddr = 0;
            if (!SafeReadU64(&f->Next, &nextAddr)) break;
            if (!nextAddr || nextAddr < 0x10000 || nextAddr > 0x7FFFFFFFFFFF) break;
            f = (UE::UField*)nextAddr;
        }
    }

    return nullptr;
}

void ProcessEvent(UE::UObject* obj, UE::UObject* func, void* params) {
    if (!obj || !func) return;

    if (g_processEventVtableIdx >= 0) {
        void** vtable = nullptr;
        if (!SafeReadU64(obj, (uint64_t*)&vtable)) return;
        uint64_t fnAddr = 0;
        if (!SafeReadU64(&vtable[g_processEventVtableIdx], &fnAddr)) return;
        auto fn = (UE::ProcessEventFn)fnAddr;
        fn(obj, func, params);
    }
    else if (g_processEvent) {
        g_processEvent(obj, func, params);
    }
    else {
        Log("[PE] ProcessEvent not available\n");
    }
}

} // namespace UEEngine
