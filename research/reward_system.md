# Deadzone Rogue Reward System - Reverse Engineering Findings

## Architecture Overview

The game uses a **Fabricator** system as the core reward/shop mechanic. After clearing waves (stages), players interact with a Fabricator that offers choices of perks, equipment, items, and services.

**Source files (from debug strings):**
- `Valhalla\Source\Valhalla\Private\Game\Fabricator\ValFabricator.cpp`
- `Valhalla\Source\Valhalla\Private\Game\Fabricator\ValFabricatorBase.cpp`
- `Valhalla\Source\Valhalla\Private\Game\Loot\ValLootManager.cpp`
- `Valhalla\Source\Valhalla\Private\Game\ValGameMode_Frontier.cpp`
- `Valhalla\Source\Valhalla\Private\Game\ValGameState_Frontier.cpp`
- `Valhalla\Source\Valhalla\Private\Game\Augments\ValAugmentComponent.cpp`
- `Valhalla\Source\Valhalla\Private\Inventory\ValInventoryComponent.cpp`
- `Valhalla\Source\Valhalla\Private\Missions\ValMission.cpp`
- `Valhalla\Source\Valhalla\Private\Missions\ValMissionComponent.cpp`
- `Valhalla\Source\Valhalla\Private\Player\Controllers\ValPlayerController.cpp`

## Key Classes and Strings

### Fabricator Option Types (UE Reflection Structs)
| String | Address |
|--------|---------|
| ValFabricatorPerkOptions | 0x149f10dc8 |
| ValFabricatorEquipmentOptions | 0x149f11f68 |
| ValFabricatorServiceOptions | 0x149f10470 |
| ValFabricatorLevelOptions | 0x149f124b8 |
| ValFabricatorRarityOptions | 0x149f131b8 |
| ValFabricatorRerollOptions | 0x149f131b8 |
| ValFabricatorItemOption | 0x149f0c150 |
| ValFabricatorLevelOption | 0x149f0ce98 |
| ValFabricatorRarityOption | 0x149f0da80 |
| ValFabricatorServiceOption | 0x149f0b500 |
| ValFabricatorHealOptions | 0x149f0f798 |
| ValFabricatorOptionCount | 0x149f227f0 |
| ValFabricatorPlayerData | 0x149f0f138 |
| ValFabricatorEffect | 0x149f21ab8 |
| FabricatorOptions | 0x149f20d60 |

### Replicated Properties (Server → Client sync)
| Property | Address | Purpose |
|----------|---------|---------|
| r_SoftFabricatorData | 0x149f1e740 | Fabricator state (server → all clients) |
| r_SoftFabricatorOptions | 0x149f1e770 | Available options (what player sees) |
| OnRep_SoftFabricatorData | 0x149f1d090 | Client callback when data arrives |
| OnRep_SoftFabricatorOptions | 0x149f1d340 | Client callback when options arrive |

### Server RPCs - Direct Grant (on PlayerController)
| Function | String Address | Purpose |
|----------|---------------|---------|
| ServerGivePerk | 0x149e89f60 | Grant a perk directly |
| ServerGiveAugment | 0x149e89a78 | Grant an augment directly |
| ServerGiveEquipment | 0x149e89ca8 | Grant equipment directly |
| ServerGrantLoot | 0x149e8bcb0 | Grant generic loot |
| ServerGiveMoney | 0x149e89de0 | Grant currency |
| ServerAddItemCount | 0x149e8f7d0 | Add items by count |
| ServerAddAmmo | 0x149e8f5f8 | Add ammo |
| ServerAddSalvageCount | 0x149e8f940 | Add salvage/scrap |

### Server RPCs - Bundle Spawning
| Function | String Address | Purpose |
|----------|---------------|---------|
| ServerSpawnEquipmentBundle | 0x149e8e4a8 | Spawn equipment pickup |
| ServerSpawnItemBundle | 0x149e8e520 | Spawn item pickup |
| ServerSpawnPerkBundle | 0x149e8e5e0 | Spawn perk bundle pickup |
| ServerSpawnSuperItemBundle | 0x149e8eac8 | Spawn superior item pickup |

### Server RPCs - Fabricator Purchase (indexed selection)
| Function | String Address | Purpose |
|----------|---------------|---------|
| ServerPurchasePerkAt | 0x149fcb650 | Buy perk at option index |
| ServerPurchaseEquipmentAt | 0x149fcafb8 | Buy equipment at index |
| ServerPurchaseHealingAt | 0x149fcb288 | Buy healing at index |
| ServerPurchaseRestockAt | 0x149fcba30 | Buy restock at index |
| ServerPurchaseUpgradeShopItem | 0x149fcbee8 | Buy upgrade |
| ServerRequestPurchase | 0x149fceb68 | Generic purchase request |

### Local Functions (non-RPC, execute locally)
| Function | String Address |
|----------|---------------|
| GivePerk | 0x149e8d620 |
| GiveAugment | 0x149e8d338 |
| GiveEquipment | 0x149e8d498 |
| SpawnItemBundle | 0x149e87f58 |
| SpawnPerkBundle | 0x149e87ff8 |
| SpawnEquipmentBundle | 0x149e87f40 |
| SpawnSuperItemBundle | 0x149e88688 |
| PurchaseEquipmentAt | 0x149f0d9a0 |
| PurchasePerkAt | 0x149f0e750 |

### Fabricator Lifecycle
| Function | String Address |
|----------|---------------|
| LoadFabricatorData | 0x149f1caf8 |
| OnFabricatorAssetsLoaded | 0x149f1cf00 |
| OnFabricatorDataLoaded | 0x149f1cf68 |
| BLUEPRINT_GenerateLoot | 0x149f1c238 |
| SetFabricatorActor | 0x149f401b8 |
| OnFabricatorInteraction | 0x149fc2430 |
| ClientNotifyFabricatorInteraction | 0x149fcce20 |
| GetCheckpointFabricatorData | 0x149e9bad8 |
| RequestPurchase | 0x149f45e18 |
| RequestPurchaseItem | 0x149f46350 |

### Wave/Stage System
| String | Address | Purpose |
|--------|---------|---------|
| OnWavesComplete | 0x149f412b8 | Callback: all waves done |
| OnCurrentWaveComplete | 0x149f412c8 | Callback: current wave done |
| SetWavesComplete | 0x149f405a0 | Set stage as complete |
| r_bWavesComplete | 0x149f49280 | Replicated: waves done flag |
| WavesPerSuperItemDrop | 0x149f1f628 | Config: how often super items drop |
| TotalSuperItemsToDrop | 0x149f1f640 | Config: total super items in run |

### Augment System
| String | Address |
|--------|---------|
| SelectDefaultAugment | 0x149f179f8 |
| ServerSelectDefaultAugment | 0x149f18b18 |
| OnRep_DefaultAugmentItems | 0x149f16928 |
| OnRep_SelectedDefaultAugmentItem | 0x149f16eb8 |
| r_DefaultAugmentItems | 0x149f1bfd8 |
| r_SelectedDefaultAugmentItem | 0x149f1c008 |
| LoadAugmentAssets | 0x149f0b3d0 |
| OnAugmentAssetsLoaded | 0x149e96478 |

### Loot System
| String | Address |
|--------|---------|
| GetValLootManager | 0x149e3e0c8 |
| ValLootItem | 0x149f47220 |
| ValLootParameters | 0x149f48938 |
| ValLootOptionData | 0x149f3d350 |
| ValLootFilter | 0x149f3b0e0 |
| ValLootRarityValues | 0x149f42670 |
| LootTable | 0x149f487c0 |
| DefaultLootTable | 0x149f36898 |
| BotLootTable | 0x149f36870 |

### Game Mode Info
| String | Address |
|--------|---------|
| EFrontierDirectionType::Fabricator | 0x149ea2048 |
| EFrontierState::Survived | 0x149ea1bf8 |
| EFrontierState::Extracted | 0x149ea1c38 |
| EPerkList::Augment | 0x14a07cc50 |
| EPerkList::PerkFamily01_Parent | 0x14a07cc68 |

## Multiplayer Flow

```
[Stage Complete]
    ↓
[OnWavesComplete / OnCurrentWaveComplete]
    ↓
[Player directed to Fabricator (EFrontierDirectionType::Fabricator)]
    ↓
[Server: LoadFabricatorData → OnFabricatorDataLoaded]
    ↓
[Server: Populates r_SoftFabricatorOptions (asset refs + metadata)]
    ↓
[UE Replication: r_SoftFabricatorOptions → all clients]
    ↓
[Client: OnRep_SoftFabricatorOptions → UI shows options]
    ↓
[Player selects option index N]
    ↓
[Client → Server RPC: ServerPurchasePerkAt(N) / ServerPurchaseEquipmentAt(N)]
    ↓
[Server validates index against its option list]
    ↓
[Server: GivePerk/GiveEquipment/GiveAugment (local exec)]
    ↓
[Item replicated to player's inventory]
```

## Recommended Hook Strategy

### Strategy A: Direct Server RPC Calls (RECOMMENDED for v1)

**Approach:** Bypass the Fabricator entirely. Call `ServerGivePerk` / `ServerGiveAugment` / `ServerGiveEquipment` directly when the user selects from our queue.

**Why this works for multiplayer:**
- `Server*` RPCs are UE's netcode mechanism — they always execute on the authority (listen server)
- When called from a client, UE serializes the call and sends it to the server
- When called from the host, it executes locally since host IS the server
- No validation of fabricator state needed — these are unconditional grants

**Implementation:**
1. At runtime, find these UFunctions via GUObjectArray (search by FName)
2. Find the local player controller instance (class name contains "ValPlayerController")
3. When user picks from queue: `ProcessEvent(playerController, serverGivePerkFunc, &perkAssetPtr)`
4. The `Server*` prefix ensures UE routes it to authority automatically

**Params struct (estimated):**
```cpp
// ServerGivePerk
struct { UObject* PerkAsset; } params;

// ServerGiveEquipment  
struct { UObject* EquipmentAsset; } params;

// ServerGiveAugment
struct { UObject* AugmentAsset; } params;

// ServerSpawnSuperItemBundle (may need location)
struct { UObject* ItemAsset; /* possibly FVector location */ } params;
```

**Pros:** Simple, works from any player, multiplayer-safe by design
**Cons:** Doesn't modify what the fabricator SHOWS (items are added outside normal selection)

### Strategy B: Hook Fabricator Option Population (v2, "control what appears")

**Approach:** Intercept `OnRep_SoftFabricatorOptions` or the server-side population to replace options with queue items.

**Implementation:**
1. Hook `OnRep_SoftFabricatorOptions` (client-side replication callback)
2. When called, modify the replicated TArray to contain our queued item asset references
3. Player sees our items in the normal fabricator UI
4. Player selects normally → `ServerPurchasePerkAt(index)` → server gives the item we injected

**Challenge:** Requires knowing exact memory layout of the option structs (`ValFabricatorPerkOptions`, etc.)

**For this to work on the server side (host player):**
- Hook `LoadFabricatorData` or `OnFabricatorDataLoaded` 
- Modify the options before they get stored in `r_SoftFabricatorOptions`
- This way the replicated data already contains our items

### Strategy C: Hybrid (BEST UX)

Combine both:
1. Use Strategy A for immediate "give me this now" (works anytime)
2. Use Strategy B for "queue this for next fabricator" (seamless UX)

## Critical Dependencies

1. **ProcessEvent must be found.** Currently failing. Alternative: use the exec function pointer directly from the UFunction object (offset ~0xB0-0xC0 in UE5.6).
2. **Asset objects must be located.** The class name filters ("ValPerkAsset", etc.) need fixing — current scan finds almost nothing. Try broader prefixes or DataAsset subclasses.
3. **Player Controller must be found.** Scan for "ValPlayerController" class instances.

## Notes on Super Items

- Super items use `ServerSpawnSuperItemBundle` — likely spawns a physical pickup
- `WavesPerSuperItemDrop` (0x149f1f628) and `TotalSuperItemsToDrop` (0x149f1f640) are config values
- These suggest super items drop at fixed intervals, not through the fabricator
- To force a super item: call `ServerSpawnSuperItemBundle` with the desired asset

## EPerkList Enum (Perk Families)

The game organizes perks into families with parent + 3 children each:
- EPerkList::PerkFamily01_Parent / Child_01 / Child_02 / Child_03
- EPerkList::PerkFamily02_Parent / Child_01 / Child_02 / Child_03
- EPerkList::PerkFamily03_Parent / Child_01 / Child_02 / Child_03
- EPerkList::Augment (augments are in the perk system)

This matches the wiki structure (Primary perk → 4 secondary perks per family).
