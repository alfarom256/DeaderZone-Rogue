#pragma once

namespace StatsOverlay {

// Initialize the stats overlay (called once during game init)
void Init();

// Heavy data polling (ProcessEvent + object scans). Call from the MOD thread only —
// never the render thread, or it hitches the game.
void Poll();

// Draw the stats overlay (render thread) — draws only the values Poll() cached.
void Render();

// Force a refresh of cached stat pointers (call after level change, respawn, etc.)
void Invalidate();

} // namespace StatsOverlay
