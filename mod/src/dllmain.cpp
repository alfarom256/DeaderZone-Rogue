#define __VERRSRC_H__
#define VER_H
#include <Windows.h>
#include <cstdio>
#include <cstring>

// ── Logging ────────────────────────────────────────────────────────
static FILE* g_log = nullptr;
static char  g_dllDir[MAX_PATH] = {};
HMODULE g_hModule = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fflush(g_log);
}

const char* GetDllDir() { return g_dllDir; }

// ── dwmapi.dll forwarding ──────────────────────────────────────────
static HMODULE g_realDwmapi = nullptr;

static FARPROC RealProc(const char* name) {
    return GetProcAddress(g_realDwmapi, name);
}

extern "C" {

typedef HRESULT(WINAPI* DwmFlush_t)();
static DwmFlush_t real_DwmFlush = nullptr;
__declspec(dllexport) HRESULT WINAPI proxy_DwmFlush() {
    if (!real_DwmFlush) real_DwmFlush = (DwmFlush_t)RealProc("DwmFlush");
    return real_DwmFlush ? real_DwmFlush() : E_FAIL;
}

typedef HRESULT(WINAPI* DwmGetCompositionTimingInfo_t)(HWND, void*);
static DwmGetCompositionTimingInfo_t real_DwmGetCompositionTimingInfo = nullptr;
__declspec(dllexport) HRESULT WINAPI proxy_DwmGetCompositionTimingInfo(HWND hwnd, void* pTimingInfo) {
    if (!real_DwmGetCompositionTimingInfo)
        real_DwmGetCompositionTimingInfo = (DwmGetCompositionTimingInfo_t)RealProc("DwmGetCompositionTimingInfo");
    return real_DwmGetCompositionTimingInfo ? real_DwmGetCompositionTimingInfo(hwnd, pTimingInfo) : E_FAIL;
}

typedef HRESULT(WINAPI* DwmIsCompositionEnabled_t)(BOOL*);
static DwmIsCompositionEnabled_t real_DwmIsCompositionEnabled = nullptr;
__declspec(dllexport) HRESULT WINAPI proxy_DwmIsCompositionEnabled(BOOL* pfEnabled) {
    if (!real_DwmIsCompositionEnabled)
        real_DwmIsCompositionEnabled = (DwmIsCompositionEnabled_t)RealProc("DwmIsCompositionEnabled");
    return real_DwmIsCompositionEnabled ? real_DwmIsCompositionEnabled(pfEnabled) : E_FAIL;
}

typedef HRESULT(WINAPI* DwmSetWindowAttribute_t)(HWND, DWORD, const void*, DWORD);
static DwmSetWindowAttribute_t real_DwmSetWindowAttribute = nullptr;
__declspec(dllexport) HRESULT WINAPI proxy_DwmSetWindowAttribute(HWND hwnd, DWORD dwAttribute, const void* pvAttribute, DWORD cbAttribute) {
    if (!real_DwmSetWindowAttribute)
        real_DwmSetWindowAttribute = (DwmSetWindowAttribute_t)RealProc("DwmSetWindowAttribute");
    return real_DwmSetWindowAttribute ? real_DwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute) : E_FAIL;
}

} // extern "C"

// ── Forward declarations ───────────────────────────────────────────
#include "hooks.h"
#include "damage_hook.h"
#include "ue_engine.h"

// ── Mod thread ─────────────────────────────────────────────────────
static DWORD WINAPI ModThread(LPVOID) {
    Log("[MOD] Thread started, waiting for game window...\n");

    HWND hwnd = nullptr;
    for (int i = 0; i < 300 && !hwnd; i++) {
        Sleep(100);
        hwnd = FindWindowW(L"UnrealWindow", nullptr);
    }
    if (!hwnd) {
        Log("[MOD] Failed to find game window after 30s, aborting\n");
        return 1;
    }
    Log("[MOD] Game window found: 0x%llX\n", (uintptr_t)hwnd);

    Sleep(3000);

    Log("[MOD] Creating D3D11 overlay...\n");
    Hooks::Init(hwnd);

    Log("[MOD] Initializing UE engine access...\n");
    if (UEEngine::Init()) {
        Log("[MOD] UE engine initialized: %s\n", UEEngine::GetInitStatus());
        Log("[MOD] Installing damage-tracker hook...\n");
        DamageHook::Init();
    } else {
        Log("[MOD] UE engine init failed: %s\n", UEEngine::GetInitStatus());
    }

    Log("[MOD] Entering render loop...\n");
    Hooks::RenderLoop();

    return 0;
}

// ── DllMain ────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;

        GetModuleFileNameA(hModule, g_dllDir, MAX_PATH);
        char* lastSlash = strrchr(g_dllDir, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';

        char logPath[MAX_PATH];
        strcpy_s(logPath, g_dllDir);
        strcat_s(logPath, "dzr_mod.log");
        g_log = fopen(logPath, "w");
        Log("[INIT] dwmapi.dll mod loaded from %s\n", g_dllDir);

        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        strcat_s(sysDir, "\\dwmapi.dll");
        g_realDwmapi = LoadLibraryA(sysDir);
        Log("[INIT] Real dwmapi.dll = 0x%llX\n", (uintptr_t)g_realDwmapi);

        if (!g_realDwmapi) {
            Log("[INIT] FATAL: Could not load real dwmapi.dll\n");
            return TRUE;
        }

        CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        DamageHook::Shutdown();
        Hooks::Shutdown();
        if (g_realDwmapi) FreeLibrary(g_realDwmapi);
        Log("[EXIT] Mod unloaded\n");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
