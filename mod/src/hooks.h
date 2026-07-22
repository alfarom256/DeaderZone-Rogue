#pragma once
#include <Windows.h>

namespace Hooks {

void Init(HWND gameWindow);
void RenderLoop();
void Shutdown();

inline bool g_menuOpen = false;

} // namespace Hooks
