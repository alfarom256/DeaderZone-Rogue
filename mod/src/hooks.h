#pragma once
#include <Windows.h>

namespace Hooks {

void Init(HWND gameWindow);
void RenderLoop();
void Shutdown();

inline bool g_menuOpen = false;

// HUD visibility, toggled with F8 during gameplay. When false the Present hook skips
// drawing the stats + damage overlays (the game frame is untouched).
inline bool g_hudVisible = true;

} // namespace Hooks
