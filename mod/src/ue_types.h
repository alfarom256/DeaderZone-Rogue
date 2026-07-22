#pragma once
#include <cstdint>
#include <string>

namespace UE {

// ── FName ───────────────────────────────────────────────────────────
struct FName {
    int32_t ComparisonIndex;
    int32_t Number;
};
static_assert(sizeof(FName) == 8);

// ── FString ─────────────────────────────────────────────────────────
struct FString {
    wchar_t* Data = nullptr;
    int32_t  Num  = 0;
    int32_t  Max  = 0;

    std::wstring ToWString() const {
        if (Data && Num > 0)
            return std::wstring(Data, Num - 1);
        return {};
    }
};

// ── GUObjectArray entry ─────────────────────────────────────────────
struct FUObjectItem {
    struct UObject* Object;
    int32_t         Flags;
    int32_t         ClusterRootIndex;
    int32_t         SerialNumber;
    int32_t         Pad;
};
static_assert(sizeof(FUObjectItem) == 24);

struct FChunkedFixedUObjectArray {
    FUObjectItem**  Objects;
    FUObjectItem*   PreAllocated;
    int32_t         MaxElements;
    int32_t         NumElements;
    int32_t         MaxChunks;
    int32_t         NumChunks;
};

struct FUObjectArray {
    int32_t         ObjFirstGCIndex;
    int32_t         ObjLastNonGCIndex;
    int32_t         MaxObjectsNotConsideredByGC;
    int32_t         OpenForDisregardForGC;
    FChunkedFixedUObjectArray ObjObjects;
};

static constexpr int OBJECTS_PER_CHUNK = 65536;

// ── UObject (minimal layout) ────────────────────────────────────────
struct UObject {
    void**   VTable;         // 0x00
    int32_t  ObjectFlags;    // 0x08
    int32_t  InternalIndex;  // 0x0C
    void*    ClassPrivate;   // 0x10  UClass*
    FName    NamePrivate;    // 0x18
    void*    OuterPrivate;   // 0x20  UObject*
};

// ── UField extends UObject ──────────────────────────────────────────
struct UField : UObject {
    void* Next; // 0x28 — UField*
};

// ── UStruct (simplified — we walk Children for UFunctions) ──────────
// In UE5, UStruct layout after UField:
//   0x30: SuperStruct (UStruct*)
//   0x38: Children    (UField* — linked list of child UFunctions)
//   0x40: ChildProperties (FField*)
//   0x48: PropertiesSize (int32_t)
//   ... more fields
// But the exact offsets depend on the build. We scan for children
// by walking the UField::Next chain starting from Children.

// ── UFunction ───────────────────────────────────────────────────────
// UFunction extends UStruct. The Func pointer (native implementation)
// is at a known offset. In UE5.6 it's typically at offset 0xB0.
// We'll determine this at runtime.

// ── RVAs from IDA + UE4SS pattern scan ──────────────────────────────
namespace RVA {
    constexpr uintptr_t GUObjectArray               = 0xB617E30;
    constexpr uintptr_t FNameToString                = 0x1396480;
    constexpr uintptr_t StaticConstructObject         = 0x15F5C50;
    constexpr uintptr_t GameEngineTick               = 0x3E6DD80;
    constexpr uintptr_t GMalloc                      = 0xB527AE8;
    constexpr uintptr_t GNatives                     = 0xB6167C0;
    constexpr uintptr_t FNameFName                   = 0x1385410;
    constexpr uintptr_t ConsoleManagerSingleton      = 0x13F8BD0;
    constexpr uintptr_t FUObjectHashTablesGet        = 0x16013F0;
}

// ProcessEvent signature: void UObject::ProcessEvent(UFunction*, void* Params)
using ProcessEventFn = void(__fastcall*)(void* thisObj, void* func, void* params);

} // namespace UE
