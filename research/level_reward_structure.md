# Deadzone Rogue - Level Reward Structure & Queue Constraints

## Run Structure (Zone 1 - Standard)

Each zone consists of **3 floors**, each floor has **3 security levels**.

### Rooms Per Security Level
| Room Type | Count | Notes |
|-----------|-------|-------|
| Item Room | 2 | Choose 1 of 3 items |
| Perk Room | 1 | Choose 1 of 3 perks (SL2+), augment at SL1 |
| Saferoom | 1 | Fabricator (buy/upgrade), superior item selection |
| Boss Room | 1 | Only on last SL of each floor; also grants perk |

### Total Sectors Per Zone: 30
- Boss at sector 10, 20, 30

### Estimated Room Sequence Per Security Level (5 rooms each = 15 per floor = 45 total? but 30 sectors per zone)

More likely interpretation: 30 sectors = rooms, distributed as:
- 3 floors × 3 SLs = 9 security levels
- Each SL ≈ 3-4 rooms (2 item + 1 perk OR augment, then saferoom between SLs)
- Bosses at sectors 10, 20, 30

## Reward Type Limits Per Run

### Augments: **1 per run**
- Awarded at Security Level 1's perk room (first perk room of the run)
- Choosing a new augment **replaces** the current one
- Only one augment can be active at any time

### Perks: **Up to 12 total** (3 primary + up to 9 secondary)
- **3 Primary Perks** - First 3 perk room selections
  - Only 1 Protective perk primary allowed
  - Only 1 Elemental perk primary allowed  
  - Multiple Combat primaries allowed
- **Up to 3 Secondary Perks per primary** = 9 max secondary
- Perk rooms start at Security Level 2
- Boss rooms also grant perks

### Items: **No hard cap documented**
- Awarded after each item room (choose 1 of 3)
- ~18 item rooms in a full 3-zone run (2 per SL × 9 SLs)
- Items persist for the run only
- Rarity: Rare or Epic

### Superior Items: **Variable, based on config**
- Binary config: `WavesPerSuperItemDrop` and `TotalSuperItemsToDrop`
- Appear at fabricator rooms (saferooms) starting in Zone 2
- Choose 1 of 3 at each fabricator
- Estimated 3-5 per run based on saferoom frequency

## Proposed Sector Sequence (Best Estimate)

For a standard 30-sector Zone 1 run:

```
Floor 1 (Sectors 1-10):
  SL1: [Item] [Item] [Augment Room] [Saferoom]     ← Sectors 1-4
  SL2: [Item] [Item] [Perk Room]    [Saferoom]     ← Sectors 5-8  
  SL3: [Item]        [Perk Room]    [BOSS @10]      ← Sectors 9-10

Floor 2 (Sectors 11-20):
  SL1: [Item] [Item] [Perk Room]    [Saferoom]     ← Sectors 11-14
  SL2: [Item] [Item] [Perk Room]    [Saferoom]     ← Sectors 15-18
  SL3: [Item]        [Perk Room]    [BOSS @20]      ← Sectors 19-20

Floor 3 (Sectors 21-30):
  SL1: [Item] [Item] [Perk Room]    [Saferoom]     ← Sectors 21-24
  SL2: [Item] [Item] [Perk Room]    [Saferoom]     ← Sectors 25-28
  SL3: [Item]        [Perk Room]    [BOSS @30]      ← Sectors 29-30
```

## Queue Constraint Rules (For Implementation)

```cpp
struct RunConstraints {
    static constexpr int MAX_AUGMENTS = 1;
    static constexpr int MAX_PRIMARY_PERKS = 3;
    static constexpr int MAX_SECONDARY_PER_PRIMARY = 3;
    static constexpr int MAX_TOTAL_PERKS = 12;  // 3 + 9
    static constexpr int MAX_ITEMS_ESTIMATE = 18; // 2 per SL * 9 SLs
    static constexpr int MAX_SUPERIOR_ITEMS = 5;  // TotalSuperItemsToDrop config
    
    // Category restrictions
    static constexpr int MAX_PROTECTIVE_PRIMARY = 1;
    static constexpr int MAX_ELEMENTAL_PRIMARY = 1;
    // Combat primaries: up to 3 (no individual limit beyond total)
};
```

### Queue Validation Rules

1. **Augments**: Queue can hold at most 1 augment. Adding a second should warn/replace.
2. **Perks**: 
   - First 3 perk slots = primary only
   - After that = secondary only (and must match a queued primary's family)
   - Max 1 protective primary, max 1 elemental primary
3. **Items**: Soft cap ~18 (no strict engine limit documented, but runs can't exceed room count)
4. **Superior Items**: Max ~5 per run (from `TotalSuperItemsToDrop` config)
5. **Queue must be cleared between runs** (user requirement)

### Slot Type Per Sector

| Sector | Reward Type |
|--------|-------------|
| 1 | Weapon (first room always gives weapon cache) |
| 2-3 | Item |
| 4 | Augment (SL1 perk room) |
| 5 | Saferoom / Superior Item (Zone 2+) |
| 6-7 | Item |
| 8 | Perk (SL2 perk room) |
| 9 | Saferoom / Superior Item |
| 10 | Boss → Perk |
| ... | Pattern repeats for floors 2 & 3 |

## Sources

- [Tribes Depot Wiki - Zones](https://wiki.tribesdepot.com/wiki/Deadzone:_Rogue/Zones)
- [Tribes Depot Wiki - Perks](https://wiki.tribesdepot.com/wiki/Deadzone:_Rogue/Perks)
- [Tribes Depot Wiki - Augments](https://wiki.tribesdepot.com/wiki/Deadzone:_Rogue/Augments)
- [Tribes Depot Wiki - Items](https://wiki.tribesdepot.com/wiki/Deadzone:_Rogue/Items)
- [Tribes Depot Wiki - Superior Items](https://wiki.tribesdepot.com/wiki/Deadzone:_Rogue/Superior_Items)
- Binary config: `WavesPerSuperItemDrop` @ 0x149f1f628, `TotalSuperItemsToDrop` @ 0x149f1f640

## Notes / Uncertainties

- The exact item cap per run is NOT documented anywhere. It appears unlimited within the number of item rooms available.
- Superior item count likely comes from the `TotalSuperItemsToDrop` binary config value (needs runtime read to confirm exact number).
- The sector sequence above is an estimate based on wiki descriptions. The exact order may vary slightly — the user or runtime observation should confirm.
- Zone 4 (Apophis expansion) may add more floors/sectors.
