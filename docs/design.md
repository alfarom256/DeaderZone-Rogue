# DeaderZoneRogue — Build Picker Mod

## Overview

A UE4SS C++ mod for Deadzone: Rogue (UE 5.5) that adds an ImGui overlay menu
toggled via F10. Players can browse and apply Perks, Superior Items, and
Augments at any time during a run to craft custom builds.

## Target Game

- **Game:** Deadzone: Rogue (Steam App 3228590)
- **Engine:** Unreal Engine 5.5
- **Internal project name:** Valhalla
- **Executable:** `Valhalla/Binaries/Win64/DeadzoneSteam-Win64-Shipping.exe`
- **Content:** IoStore (ucas/utoc) + pak
- **Install path:** `D:\SteamLibrary\steamapps\common\Deadzone Rogue`
- **Anti-cheat:** None detected

## Architecture

```
UE4SS (dwmapi.dll proxy)
  ├── Injects into game process
  ├── Hooks UE reflection system (GObjects, GNames, GWorld)
  ├── Provides ImGui overlay (in-game, toggle key)
  └── Loads C++ mods from ue4ss/Mods/
        └── DeaderZoneRogue.dll (our mod)
              ├── Registers ImGui tab "Build Picker"
              ├── on_unreal_init: discovers game classes
              ├── Enumerates available perks/items/augments
              └── Applies selections to player character
```

## Menu Design

Three tabs: Perks, Superior Items, Augments. Each tab has:
- Search/filter bar
- Scrollable checklist of all available options
- Apply Selected / Clear All buttons

Toggle: F10 opens UE4SS overlay → our tab is visible.

## Distribution

Friends install by:
1. Copying UE4SS files into `Valhalla/Binaries/Win64/`
2. Copying mod DLL into `ue4ss/Mods/DeaderZoneRogue/`
3. Enabling mod in `ue4ss/Mods/mods.txt`

## Phases

### Phase 1: Setup & Discovery
- Install UE4SS experimental into the game
- Run SDK dumper to generate UHT headers
- Use live object browser to find class names for perks, items, augments
- Identify player character class and inventory/loadout storage

### Phase 2: Mod Implementation
- CMake project linked against UE4SS
- CppUserModBase subclass with ImGui tab
- Enumerate game data (perks, items, augments) from UE reflection
- Implement give/remove logic per category

### Phase 3: Polish & Package
- Filter/search functionality
- Persist selections across menu opens
- Package for easy distribution
- Test in co-op

## Technical Unknowns (Phase 1)
- Exact class/struct names for perks, items, augments
- How the player's loadout is stored (component? array? map?)
- Whether items are defined in DataTables, Blueprints, or native classes
- Whether changes replicate in co-op or need server authority
