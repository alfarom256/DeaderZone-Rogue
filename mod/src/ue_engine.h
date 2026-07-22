#pragma once
#include "ue_types.h"
#include <string>
#include <vector>
#include <functional>

#ifdef GetClassName
#undef GetClassName
#endif

namespace UEEngine {

bool Init();

uintptr_t GetModuleBase();

std::wstring FNameToString(UE::FName name);
UE::FName FNameFromString(const wchar_t* str);
// Find-only lookup: returns {0,0} if the name was never registered (safe for asset names)
UE::FName FNameFindExisting(const wchar_t* str);

void ForEachObject(const std::function<bool(UE::UObject*)>& callback);

UE::UObject* FindObject(const wchar_t* className, const wchar_t* objectName = nullptr);

std::vector<UE::UObject*> FindObjectsOfClass(const wchar_t* className);

std::wstring GetClassName(UE::UObject* obj);

std::wstring GetObjectName(UE::UObject* obj);

std::wstring GetFullName(UE::UObject* obj);

// Find a UFunction child of a UClass by name
UE::UObject* FindFunction(UE::UObject* classObj, const wchar_t* funcName);

// Call ProcessEvent on a UObject
void ProcessEvent(UE::UObject* obj, UE::UObject* func, void* params);

// Check if ProcessEvent was found
bool HasProcessEvent();

// Free an FString buffer allocated by the engine
void FreeFString(UE::FString& str);

// Game allocator (GMalloc) access — for growing engine-owned TArrays in place.
void* GameMalloc(size_t size);
void* GameRealloc(void* original, size_t size);

// Debug: get status info
int GetObjectCount();
const char* GetInitStatus();

} // namespace UEEngine
