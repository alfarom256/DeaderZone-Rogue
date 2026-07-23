#include "stats_overlay.h"
#include "damage_tracker.h"
#include "ue_engine.h"
#include <Windows.h>
#include <imgui.h>
#include <cstring>
#include <chrono>

#ifdef GetClassName
#undef GetClassName
#endif

extern void Log(const char* fmt, ...);

namespace StatsOverlay {

// Property names on ValPlayerStatsComponent (discovered via IDA binary analysis)
struct StatDef {
    const char* displayName;
    const wchar_t* propName;
    enum Format { Multiplier, Modifier, Chance, Absolute } format;
    int32_t cachedOffset;
    float   value;
    bool    found;
};

static StatDef g_stats[] = {
    {"Max Health",       L"HealthMax",                       StatDef::Absolute,   -1, 0, false},
    {"Max Shield",       L"ShieldMax",                       StatDef::Absolute,   -1, 0, false},
    {"Crit Chance",      L"CriticalChanceModifier",          StatDef::Modifier,   -1, 0, false},
    {"Crit Damage",      L"CriticalDamageMultiplier",        StatDef::Multiplier, -1, 0, false},
    {"Proc Chance",      L"UniversalProcChanceMultiplier",   StatDef::Multiplier, -1, 0, false},
    {"Proc Damage",      L"ProcDamageMultiplier",            StatDef::Multiplier, -1, 0, false},
    {"Weakpoint Dmg",    L"WeakpointDamageModifier",         StatDef::Modifier,   -1, 0, false},
    {"Dodge Chance",     L"DodgeChance",                     StatDef::Chance,     -1, 0, false},
    {"Armor Rating",     L"ArmorRatingMultiplier",           StatDef::Multiplier, -1, 0, false},
    {"Damage",           L"GlobalDamageMultiplier",          StatDef::Multiplier, -1, 0, false},
    {"Weapon Damage",    L"GlobalAllWeaponsDamageMultiplier",StatDef::Multiplier, -1, 0, false},
    {"Melee Damage",     L"GlobalMeleeDamageMultiplier",     StatDef::Multiplier, -1, 0, false},
    {"Grenade Damage",   L"GrenadeDamageMultiplier",         StatDef::Multiplier, -1, 0, false},
    {"Fire Rate",        L"AttackRateMultiplier",            StatDef::Multiplier, -1, 0, false},
    {"Reload Speed",     L"ReloadSpeed",                     StatDef::Multiplier, -1, 0, false},
    {"Move Speed",       L"MovementSpeed",                   StatDef::Multiplier, -1, 0, false},
    {"Sprint Speed",     L"SprintSpeedMultiplier",           StatDef::Multiplier, -1, 0, false},
    {"Headshot Dmg",     L"HeadshotDamageModifier",          StatDef::Modifier,   -1, 0, false},
    // Hidden — drives the damage tracker from the game's cumulative counter.
    {"__DamageDealt",    L"DamageDealt",                     StatDef::Absolute,   -1, 0, false},
};
static constexpr int kNumStats = sizeof(g_stats) / sizeof(g_stats[0]);

// Raw BaseValue (+0x8, FGameplayAttributeData) per stat. NOTE: permanent character upgrades
// live in CurrentValue, NOT here (confirmed in-game: Max Health base=100, cur=300 with the
// health perk maxed). So raw Base does NOT include upgrades and is only kept for reference.
static float g_statBase[kNumStats] = {0};

// "Effective base" = CurrentValue captured while in the LOBBY (raw + character upgrades, no
// run pickups). yellow (in-run gear/items) = CurrentValue - effective base. Tracked live in
// the lobby so it absorbs every perk purchase; frozen during a run so pickups show as yellow.
static float g_statBaseVal[kNumStats] = {0};
static bool  g_statBaseSet[kNumStats] = {false};

static float    g_statPrev[kNumStats]     = {0};       // previous value (for change detection)
static bool     g_statPrevInit[kNumStats] = {false};
static uint64_t g_statFlashMs[kNumStats]  = {0};       // last-change timestamp (subtle flash)

// One-shot (re-armed on pawn change): dump Base vs Current for every stat so the yellow
// split is confirmed from data, not assumed.
static bool     g_statbcDump = true;

// Lobby vs active run, from the authoritative GameState class (lobby hub uses a distinct
// GameState — BP_MainMenuGameState_C — from an in-run GameState).
static bool     g_inLobby = true;

static UE::UObject* g_statsComponent = nullptr;
static void*        g_statsClass     = nullptr;   // class ptr at bind — detects freed-and-reused memory
static bool         g_initialized    = false;
static bool         g_scanComplete   = false;
static bool         g_propsResolved  = false;

static std::chrono::steady_clock::time_point g_lastPoll = std::chrono::steady_clock::now();
static constexpr auto kPollInterval = std::chrono::milliseconds(250);

// ── Safe memory access ──────────────────────────────────────────────────────

static bool SafeReadFloat(const void* addr, float* out) {
    __try {
        memcpy(out, addr, sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadU64(const void* addr, uint64_t* out) {
    __try {
        memcpy(out, addr, sizeof(uint64_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadI32(const void* addr, int32_t* out) {
    __try {
        memcpy(out, addr, sizeof(int32_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadFName(const void* addr, UE::FName* out) {
    __try {
        memcpy(out, addr, sizeof(UE::FName));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool IsValidPtr(uint64_t ptr) {
    return (ptr > 0x10000 && ptr < 0x7FFFFFFFFFFF);
}

static bool IsValidObject(UE::UObject* obj) {
    if (!obj) return false;
    if (!IsValidPtr((uint64_t)(uintptr_t)obj)) return false;
    uint64_t vtable = 0;
    if (!SafeReadU64(obj, &vtable)) return false;
    return IsValidPtr(vtable);
}

// ── Object scanning ─────────────────────────────────────────────────────────

// Does this object's class hierarchy contain a property with this exact name?
// Used to identify the COMBAT stats component (the loose name "ValPlayerStats"
// matched a leaderboard component with PlayerStatsMap instead).
static bool ClassHasProp(UE::UObject* obj, const wchar_t* want) {
    void* cls = nullptr;
    if (!SafeReadU64(&obj->ClassPrivate, (uint64_t*)&cls)) return false;
    uint8_t* cur = (uint8_t*)cls;
    int depth = 0;
    while (cur && IsValidPtr((uint64_t)(uintptr_t)cur) && depth < 10) {
        depth++;
        for (int cpOff : {0x50, 0x58, 0x48, 0x60, 0x40}) {
            uint64_t fa = 0;
            if (!SafeReadU64(cur + cpOff, &fa) || !IsValidPtr(fa)) continue;
            uint8_t* f = (uint8_t*)(uintptr_t)fa; int w = 0;
            while (f && w < 600) {
                w++;
                UE::FName fn = {};
                if (!SafeReadFName(f + 0x20, &fn)) break;
                if (UEEngine::FNameToString(fn) == want) return true;
                uint64_t nx = 0;
                if (!SafeReadU64(f + 0x18, &nx) || !IsValidPtr(nx)) break;
                f = (uint8_t*)(uintptr_t)nx;
            }
            if (w > 2) break;
        }
        uint64_t sup = 0; bool fs = false;
        for (int so : {0x40, 0x48, 0x30}) {
            if (SafeReadU64(cur + so, &sup) && IsValidPtr(sup) &&
                (uint8_t*)(uintptr_t)sup != cur) { cur = (uint8_t*)(uintptr_t)sup; fs = true; break; }
        }
        if (!fs) break;
    }
    return false;
}

// ── Resolve the LOCAL player (ME) — multiplayer-correct ──────────────────────
// In multiplayer there are many player pawns/attribute sets. GetPlayerPawn/
// Controller/State(world, 0) always resolve to the LOCAL player (index 0 is the
// machine's own player on both client and listen-server host), so this is "me",
// never a teammate. We match the attribute set whose owner chain reaches any of
// these — the ASC may hang off the PlayerState rather than the pawn.
struct LocalActors { UE::UObject* pawn; UE::UObject* controller; UE::UObject* state; UE::UObject* gameState; };

static UE::UObject* FindObjByExactName(const wchar_t* name) {
    UE::UObject* r = nullptr;
    UEEngine::ForEachObject([&](UE::UObject* o) -> bool {
        if (UEEngine::GetObjectName(o) == name) { r = o; return false; }
        return true;
    });
    return r;
}

static UE::UObject* FindNonCDOByClassSubstr(const wchar_t* sub) {
    UE::UObject* r = nullptr;
    UEEngine::ForEachObject([&](UE::UObject* o) -> bool {
        std::wstring cn = UEEngine::GetClassName(o);
        if (cn.find(sub) == std::wstring::npos) return true;
        std::wstring on = UEEngine::GetObjectName(o);
        if (on.find(L"Default__") != std::wstring::npos) return true;
        r = o; return false;
    });
    return r;
}

// Call a UGameplayStatics::GetPlayerX(WorldContext, 0) static and return the result.
static UE::UObject* CallGetLocalActor(UE::UObject* gsCDO, UE::UObject* worldCtx,
                                      const wchar_t* fnName) {
    if (!gsCDO || !worldCtx || !UEEngine::HasProcessEvent()) return nullptr;
    void* cls = nullptr;
    if (!SafeReadU64(&gsCDO->ClassPrivate, (uint64_t*)&cls) || !cls) return nullptr;
    UE::UObject* fn = UEEngine::FindFunction((UE::UObject*)cls, fnName);
    if (!fn) return nullptr;
    // GetPlayerX layout: WorldContextObject@0x0, PlayerIndex@0x8, ReturnValue@0x10.
    struct { void* wco; int32_t idx; int32_t pad; void* ret; } p = { worldCtx, 0, 0, nullptr };
    UEEngine::ProcessEvent(gsCDO, fn, &p);
    return (UE::UObject*)p.ret;
}

// UGameplayStatics::GetGameState(WorldContext) — 1-arg static (ReturnValue@0x8, NOT the
// 2-arg GetPlayerX layout). Authoritative current-world GameState; used as the lobby/run
// signal (its class distinguishes the hub from an active run).
static UE::UObject* CallGetGameState(UE::UObject* gsCDO, UE::UObject* worldCtx) {
    if (!gsCDO || !worldCtx || !UEEngine::HasProcessEvent()) return nullptr;
    void* cls = nullptr;
    if (!SafeReadU64(&gsCDO->ClassPrivate, (uint64_t*)&cls) || !cls) return nullptr;
    UE::UObject* fn = UEEngine::FindFunction((UE::UObject*)cls, L"GetGameState");
    if (!fn) return nullptr;
    struct { void* wco; void* ret; } p = { worldCtx, nullptr };
    UEEngine::ProcessEvent(gsCDO, fn, &p);
    return (UE::UObject*)p.ret;
}

static LocalActors ResolveLocalActors() {
    LocalActors la = {};
    UE::UObject* gsCDO = FindObjByExactName(L"Default__GameplayStatics");
    UE::UObject* worldCtx = FindNonCDOByClassSubstr(L"PlayerController");
    if (!gsCDO || !worldCtx) return la;
    la.pawn       = CallGetLocalActor(gsCDO, worldCtx, L"GetPlayerPawn");
    la.controller = CallGetLocalActor(gsCDO, worldCtx, L"GetPlayerController");
    la.state      = CallGetLocalActor(gsCDO, worldCtx, L"GetPlayerState");
    la.gameState  = CallGetGameState(gsCDO, worldCtx);
    return la;
}

// Does obj's outer chain (walking +0x20) reach any of the local player's actors?
static bool OwnerChainReaches(UE::UObject* obj, const LocalActors& la) {
    UE::UObject* o = obj;
    for (int d = 0; d < 6 && o; d++) {
        if ((la.pawn && o == la.pawn) || (la.controller && o == la.controller) ||
            (la.state && o == la.state))
            return true;
        uint64_t next = 0;
        if (!SafeReadU64((uint8_t*)o + 0x20, &next)) break;
        o = (UE::UObject*)(uintptr_t)next;
        if (!IsValidObject(o)) break;
    }
    return false;
}

// ── Authoritative per-run damage total (ValPlayerStatsComponent) ─────────────
// The end-screen "damage dealt" lives in ValPlayerStatsComponent's FFastArraySerializer
// (NOT the GAS attribute set): items@+0x200, count@+0x208, 32-byte entries, key@+0x0C
// (8B ptr), int64 value@+0x18, ready-gate@+0xF0. The damage entry's key = *(reg+0xDC0)
// where reg = call sub_145D60000. It sums every damage application (DoT/AoE/proc/pet),
// matches the game's number, updates live, and resets each run. Re-resolved (not cached
// across runs) so it auto-clears on run restart.
static UE::UObject* g_dmgStatsComp = nullptr;
static uint64_t     g_dmgKey       = 0;

static uint64_t ResolveDamageKey() {
    uintptr_t base = UEEngine::GetModuleBase();
    if (!base) return 0;
    void* reg = nullptr;
    __try { reg = ((void* (*)())(base + 0x5D60000))(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!reg || !IsValidPtr((uint64_t)(uintptr_t)reg)) return 0;
    uint64_t key = 0;
    SafeReadU64((uint8_t*)reg + 0xDC0, &key);
    return key;
}

static void FindDamageStatsComponent(const LocalActors& la) {
    g_dmgStatsComp = nullptr;
    UEEngine::ForEachObject([&la](UE::UObject* o) -> bool {
        std::wstring on = UEEngine::GetObjectName(o);
        if (on.find(L"Default__") != std::wstring::npos) return true;
        std::wstring cn = UEEngine::GetClassName(o);
        if (cn.find(L"ValPlayerStatsComponent") == std::wstring::npos) return true;
        if (!OwnerChainReaches(o, la)) return true;
        g_dmgStatsComp = o;
        Log("[DMGSTAT] bound ValPlayerStatsComponent %p (local player)\n", o);
        return false;
    });
}

// Read the local player's current-run total damage. Returns false if unavailable.
static bool ReadRunDamageTotal(double* out) {
    if (!g_dmgStatsComp || !IsValidObject(g_dmgStatsComp)) return false;
    uint8_t* comp = (uint8_t*)g_dmgStatsComp;
    int32_t ready = 0;
    if (!SafeReadI32(comp + 0xF0, &ready) || (ready & 0xFF) == 0) return false;  // not replicated yet
    uint64_t items = 0; int32_t num = 0;
    if (!SafeReadU64(comp + 0x200, &items) || !SafeReadI32(comp + 0x208, &num)) return false;
    if (!IsValidPtr(items) || num <= 0 || num > 8192) return false;

    for (int i = 0; i < num; i++) {
        uint8_t* e = (uint8_t*)(uintptr_t)items + (size_t)i * 32;
        uint64_t key = 0;
        if (!SafeReadU64(e + 0x0C, &key)) break;
        if (key == g_dmgKey) {
            uint64_t val = 0;
            if (SafeReadU64(e + 0x18, &val)) { *out = (double)(int64_t)val; return true; }
        }
    }
    return false;
}

// Poll the game's lifetime damage counter and feed the tracker, which displays the
// per-run delta (increase since run start). Re-baseline when the local pawn changes
// (respawn / new level) so "damage this run" restarts correctly.
// BINDING ONLY (called at the 250ms cadence). The actual damage READ happens faster in
// Poll() for responsive burst DPS. Run-boundary reset is handled by PollStats (GameState).
static void PollRunDamage(const LocalActors& la) {
    if (!g_dmgKey) g_dmgKey = ResolveDamageKey();
    if (!g_dmgKey) return;
    if (!g_dmgStatsComp || !IsValidObject(g_dmgStatsComp)) {
        if (la.pawn || la.controller || la.state) {
            FindDamageStatsComponent(la);
            if (g_dmgStatsComp) DamageTracker::MarkDiscontinuity();  // don't count the rebind gap as a burst
        }
    }
}

// Fast, cheap damage read (memory-only, no ProcessEvent/scan) for accurate burst DPS.
static void ReadDamageFast() {
    if (!g_dmgKey || !g_dmgStatsComp || !IsValidObject(g_dmgStatsComp)) return;
    double lifetime = 0.0;
    if (ReadRunDamageTotal(&lifetime)) DamageTracker::SetCumulative(lifetime);
}

// GEAR PROBE: gear stat affixes are NOT in the attribute set — they live on the equipped
// items. Locate them by scanning every LOCAL-owned inventory/armor/item/weapon object for the
// user's KNOWN affix magnitudes (green chest +6 Max Health, green boots +2 Max Shield). Each
// 6.0/2.0 hit is logged with adjacent-qword context so we can identify the affix struct
// (target FProperty + magnitude) and fold gear into "blue".
static bool NearF(float v, float t) { float d = v - t; return d < 0.02f && d > -0.02f; }

static void ProbeInventory(const LocalActors& la) {
    int scanned = 0;
    UEEngine::ForEachObject([&](UE::UObject* o) -> bool {
        std::wstring cn = UEEngine::GetClassName(o);
        bool gear = cn.find(L"Inventory") != std::wstring::npos || cn.find(L"Armor")  != std::wstring::npos
                 || cn.find(L"Equipment") != std::wstring::npos || cn.find(L"Loadout")!= std::wstring::npos
                 || cn.find(L"Item")      != std::wstring::npos || cn.find(L"Weapon") != std::wstring::npos
                 || cn.find(L"Gear")      != std::wstring::npos;
        if (!gear) return true;
        if (UEEngine::GetObjectName(o).find(L"Default__") != std::wstring::npos) return true;
        if (!OwnerChainReaches(o, la)) return true;
        uint8_t* c = (uint8_t*)o;
        scanned++;
        for (int off = 0x28; off < 0x1800; off += 4) {
            float f = 0.0f;
            if (!SafeReadFloat(c + off, &f)) continue;
            if (NearF(f, 6.0f) || NearF(f, 2.0f)) {
                int a = off & ~7;
                uint64_t qm = 0, qp = 0;
                SafeReadU64(c + a - 8, &qm);   // qword before the aligned slot
                SafeReadU64(c + a + 8, &qp);   // qword after
                std::wstring pc;
                if (IsValidObject((UE::UObject*)(uintptr_t)qm)) pc = UEEngine::GetClassName((UE::UObject*)(uintptr_t)qm);
                Log("[GEARSCAN] %ls +0x%X=%.2f  pre=%llX(%ls) post=%llX\n",
                    cn.c_str(), off, f, (unsigned long long)qm, pc.c_str(), (unsigned long long)qp);
            }
        }
        return true;
    });
    Log("[GEARSCAN] scanned %d local gear/item objects\n", scanned);
}

// ── DIAGNOSTIC: enumerate every live UValAttributeSet ────────────────────────
// The stats read frozen at 1.0 because we bind the FIRST matching attribute set,
// but in a level every character (player + enemies) plus templates own one. This
// dumps all non-CDO instances with their owner (outer chain) and their live values
// at the agent-verified GAS offsets: GlobalDamageMultiplier CurrentValue = set+0x1BC,
// UniversalProcChanceMultiplier CurrentValue = set+0xDBC (BaseValue at -4). Pick up a
// buff and whichever instance's CurrentValue moves — and its owner — is the player's.
static void DumpAllAttributeSets() {
    LocalActors la = ResolveLocalActors();
    Log("[ATTRSET] local pawn=%p controller=%p state=%p\n", la.pawn, la.controller, la.state);

    // CHEAT-PATH PROBE (read-only): AGameModeBase::AllowCheats() returns true only in
    // NM_Standalone, and ValCheatComponent auto-creates on the PlayerController (class@
    // +0xC60, instance@+0xC68) only when AllowCheats passed. So a NON-NULL instance in a
    // solo level == cheats enabled == the game's own ServerGiveEquipment grant path is
    // usable. NULL == the session is networked (listen-server/RallyHere) and the gate is
    // shut. This is the one runtime fact static analysis can't give us.
    if (la.controller) {
        uint64_t cheatClass = 0, cheatInst = 0;
        SafeReadU64((uint8_t*)la.controller + 0xC60, &cheatClass);
        SafeReadU64((uint8_t*)la.controller + 0xC68, &cheatInst);
        Log("[CHEATPROBE] PC=%p cheatClass@+0xC60=%llX cheatInst@+0xC68=%llX  => %s\n",
            la.controller, (unsigned long long)cheatClass, (unsigned long long)cheatInst,
            cheatInst ? "STANDALONE/cheats-ON (ServerGiveEquipment usable)"
                      : "networked/cheats-OFF (gate shut)");
    }
    int idx = 0;
    UEEngine::ForEachObject([&idx, &la](UE::UObject* obj) -> bool {
        std::wstring on = UEEngine::GetObjectName(obj);
        if (on.find(L"Default__") != std::wstring::npos) return true;   // skip CDO
        std::wstring cn = UEEngine::GetClassName(obj);
        if (cn.find(L"Stat") == std::wstring::npos &&
            cn.find(L"Attribute") == std::wstring::npos) return true;
        if (!ClassHasProp(obj, L"GlobalDamageMultiplier")) return true;

        // Owner chain: walk OuterPrivate (UObject+0x20) a few levels.
        char owner[256]; int p = 0;
        UE::UObject* o = obj;
        for (int d = 0; d < 4 && o; d++) {
            uint64_t nextOuter = 0;
            SafeReadU64((uint8_t*)o + 0x20, &nextOuter);
            UE::UObject* outer = (UE::UObject*)(uintptr_t)nextOuter;
            if (!IsValidObject(outer)) break;
            std::wstring ocn = UEEngine::GetClassName(outer);
            std::wstring oon = UEEngine::GetObjectName(outer);
            p += snprintf(owner + p, sizeof(owner) - p, " <- %ls:%ls",
                          ocn.c_str(), oon.c_str());
            o = outer;
        }

        float gdBase = 0, gdCur = 0, pcBase = 0, pcCur = 0;
        SafeReadFloat((uint8_t*)obj + 0x1B8, &gdBase);
        SafeReadFloat((uint8_t*)obj + 0x1BC, &gdCur);
        SafeReadFloat((uint8_t*)obj + 0xDB8, &pcBase);
        SafeReadFloat((uint8_t*)obj + 0xDBC, &pcCur);
        bool mine = OwnerChainReaches(obj, la);
        idx++;
        if (mine)   // only log MY set(s), not all ~190 (that spam caused stutter)
            Log("[ATTRSET #%d] <<< MINE %ls:%ls  GlobalDmg base=%.3f cur=%.3f  Proc base=%.3f cur=%.3f  owner:%s\n",
                idx, cn.c_str(), on.c_str(), gdBase, gdCur, pcBase, pcCur, owner);
        return true;
    });
    Log("[ATTRSET] scanned %d attribute sets total\n", idx);
}

static void ScanForStatsComponent() {
    g_statsComponent = nullptr;

    // MULTIPLAYER-CORRECT: every character (player + all enemies + teammates) owns a
    // UValAttributeSet, so first-match binds a random enemy. Bind ONLY the set whose
    // owner chain reaches the LOCAL player (me) — verified in-game: the MINE-tagged set
    // (owner BP_CharPlayer_Dungeon_C) showed Proc cur=1.5 (a live buff), proving this
    // is the right instance and CurrentValue(+12) reflects buffs.
    LocalActors la = ResolveLocalActors();
    if (!la.pawn && !la.controller && !la.state) {
        // Not in a level yet — can't identify "me". Retry next frame (don't complete).
        return;
    }

    UEEngine::ForEachObject([&la](UE::UObject* obj) -> bool {
        std::wstring on = UEEngine::GetObjectName(obj);
        if (on.find(L"Default__") != std::wstring::npos) return true;  // skip CDO
        std::wstring cn = UEEngine::GetClassName(obj);
        if (cn.find(L"Stat") == std::wstring::npos &&
            cn.find(L"Attribute") == std::wstring::npos) return true;
        if (!ClassHasProp(obj, L"GlobalDamageMultiplier")) return true;
        if (!OwnerChainReaches(obj, la)) return true;   // <-- only MY set, not enemies'
        g_statsComponent = obj;
        SafeReadU64(&obj->ClassPrivate, (uint64_t*)&g_statsClass);  // for staleness checks
        Log("[STATS] Bound MY attribute set at 0x%llX (owner reaches local player)\n",
            (uint64_t)(uintptr_t)obj);
        return false;
    });

    if (g_statsComponent) {
        g_scanComplete = true;
        Log("[STATS] Stats component ready (local player)\n");
    }
    // else: my set not spawned yet — leave scanComplete false so we retry next frame.
}

// ── FProperty chain walking ─────────────────────────────────────────────────

static int g_resolveAttempts = 0;

static void ResolvePropertyOffsets() {
    if (g_propsResolved) return;
    if (!g_statsComponent) return;

    g_resolveAttempts++;
    // Only log details on first attempt to avoid spam
    bool verbose = (g_resolveAttempts == 1);

    void* classPtr = nullptr;
    if (!SafeReadU64(&g_statsComponent->ClassPrivate, (uint64_t*)&classPtr)) return;
    if (!classPtr || !IsValidPtr((uint64_t)(uintptr_t)classPtr)) return;

    if (verbose)
        Log("[STATS] Resolving properties from class at 0x%llX\n", (uint64_t)(uintptr_t)classPtr);

    // Walk class hierarchy (class + super classes)
    uint8_t* currentClass = (uint8_t*)classPtr;
    int classDepth = 0;
    int totalPropsWalked = 0;

    while (currentClass && IsValidPtr((uint64_t)(uintptr_t)currentClass) && classDepth < 10) {
        classDepth++;

        // UStruct::ChildProperties — try common offsets for UE5
        static const int kChildPropsOffsets[] = {0x50, 0x58, 0x48, 0x60, 0x40};

        for (int cpOff : kChildPropsOffsets) {
            uint64_t fieldAddr = 0;
            if (!SafeReadU64(currentClass + cpOff, &fieldAddr)) continue;
            if (!IsValidPtr(fieldAddr)) continue;

            uint8_t* field = (uint8_t*)(uintptr_t)fieldAddr;
            int walkCount = 0;
            int chainLen = 0;

            while (field && walkCount < 500) {
                walkCount++;
                chainLen++;

                // FField+0x28: FName NamePrivate
                UE::FName fname = {};
                if (!SafeReadFName(field + 0x20, &fname)) break;

                std::wstring propName = UEEngine::FNameToString(fname);
                if (propName.empty() || propName == L"<crash>") break;

                totalPropsWalked++;

                // Log first 30 properties we encounter on first attempt
                if (verbose && totalPropsWalked <= 30) {
                    int32_t propOffset = 0;
                    SafeReadI32(field + 0x44, &propOffset);
                    Log("[STATS]   [depth%d+0x%X] %ls offset=0x%X\n",
                        classDepth, cpOff, propName.c_str(), propOffset);
                }

                // One-shot defensive-attribute sweep: find the REAL damage-reduction / armor /
                // dodge attributes (binary strings show ArmorDamageReduction, ArmorRating*,
                // DamageResistanceModifiers) so we can add a correct DR stat and fix the inert ones.
                if (verbose) {
                    auto has = [&](const wchar_t* s){ return propName.find(s) != std::wstring::npos; };
                    if (has(L"Armor") || has(L"Dodge") || has(L"Mitigat") || has(L"Reduc") ||
                        has(L"Resist") || has(L"Defen") || has(L"Evas") || has(L"Block") || has(L"Tough")) {
                        int32_t o = 0; SafeReadI32(field + 0x44, &o);
                        Log("[ATTRLIST] %ls off=0x%X\n", propName.c_str(), o);
                    }
                }

                for (int i = 0; i < kNumStats; i++) {
                    if (g_stats[i].cachedOffset >= 0) continue;
                    if (propName == g_stats[i].propName) {
                        int32_t propOffset = 0;
                        if (SafeReadI32(field + 0x44, &propOffset)) {   // FProperty offset field
                            if (propOffset > 0 && propOffset < 0x4000) {
                                g_stats[i].cachedOffset = propOffset;
                                Log("[STATS]   MATCH: %ls -> 0x%X\n", propName.c_str(), propOffset);
                            }
                        }
                        break;
                    }
                }

                // FField+0x20: Next
                uint64_t nextAddr = 0;
                if (!SafeReadU64(field + 0x18, &nextAddr)) break;
                if (!IsValidPtr(nextAddr)) break;
                field = (uint8_t*)(uintptr_t)nextAddr;
            }

            // If we walked a valid chain (>2 properties), this is the right ChildProps offset
            if (chainLen > 2) {
                if (verbose)
                    Log("[STATS]   ChildProps at +0x%X: %d properties in chain\n", cpOff, chainLen);

                // Check if we found anything
                bool foundAny = false;
                for (int i = 0; i < kNumStats; i++) {
                    if (g_stats[i].cachedOffset >= 0) { foundAny = true; break; }
                }
                if (foundAny) goto done_walking;
                // Even if nothing matched, don't try other offsets on same class
                break;
            }
        }

        // Walk to SuperStruct (UStruct+0x30 or +0x40 or +0x48)
        uint64_t superAddr = 0;
        bool foundSuper = false;
        for (int sOff : {0x40, 0x48, 0x30}) {
            if (SafeReadU64(currentClass + sOff, &superAddr) && IsValidPtr(superAddr) &&
                (uint8_t*)(uintptr_t)superAddr != currentClass) {
                currentClass = (uint8_t*)(uintptr_t)superAddr;
                foundSuper = true;
                break;
            }
        }
        if (!foundSuper) break;
    }

done_walking:
    int resolved = 0;
    for (int i = 0; i < kNumStats; i++) {
        if (g_stats[i].cachedOffset >= 0) resolved++;
    }

    if (verbose || resolved > 0)
        Log("[STATS] Resolved %d/%d properties (walked %d props across %d classes)\n",
            resolved, kNumStats, totalPropsWalked, classDepth);

    // If we walked properties but found nothing, mark as done to stop spamming
    if (totalPropsWalked > 0) {
        g_propsResolved = true;
    }
}

// ── Stat polling ────────────────────────────────────────────────────────────

static void PollStats() {
    LocalActors la = ResolveLocalActors();

    // NEW LIFE/LEVEL: the local pawn changed => the attribute set owned by the old (now
    // freed) pawn is stale. Proactively drop the cached pointer so we never read garbage
    // ("stats poll invalid memory then fix themselves"). Re-resolves to the new pawn's set.
    static UE::UObject* s_lastPawn = nullptr;
    if (la.pawn && la.pawn != s_lastPawn) {
        s_lastPawn = la.pawn;
        g_statsComponent = nullptr; g_statsClass = nullptr;
        g_scanComplete = false; g_propsResolved = false;
        for (int i = 0; i < kNumStats; i++) {
            g_stats[i].found = false; g_stats[i].cachedOffset = -1; g_stats[i].value = 0.0f;
        }
        g_dmgStatsComp = nullptr;   // damage component can be freed too -> re-resolve or it reads a frozen value
        g_statbcDump = true;        // re-dump Base/Current for the new map (lobby vs run)
        // DIAGNOSTIC: log the World (map) name — a candidate run/lobby-boundary signal.
        std::wstring worldName;
        { UE::UObject* o = la.pawn;
          for (int d = 0; d < 6 && o; d++) {
              if (UEEngine::GetClassName(o).find(L"World") != std::wstring::npos) { worldName = UEEngine::GetObjectName(o); break; }
              uint64_t nx = 0; if (!SafeReadU64((uint8_t*)o + 0x20, &nx)) break;
              o = (UE::UObject*)(uintptr_t)nx; if (!IsValidObject(o)) break;
          } }
        Log("[STATS] pawn changed -> re-resolving (pawn=%p world=%ls)\n", (void*)la.pawn, worldName.c_str());
    }

    // LOBBY vs RUN, from the authoritative UGameplayStatics::GetGameState. The hub uses a
    // distinct GameState (BP_MainMenuGameState_C) from an active run; this drives the yellow
    // base capture below and the per-run damage reset. Anchored on lobby/menu tokens so it's
    // robust to whatever the run's GameState class is named.
    if (la.gameState && IsValidObject(la.gameState)) {
        std::wstring gcn = UEEngine::GetClassName(la.gameState);
        g_inLobby = gcn.find(L"MainMenu") != std::wstring::npos
                 || gcn.find(L"Lobby")    != std::wstring::npos
                 || gcn.find(L"FrontEnd") != std::wstring::npos;
        static UE::UObject* s_lastGSInst = nullptr;
        if (la.gameState != s_lastGSInst) {
            Log("[GS] GameState %p (%ls) => %s\n", (void*)la.gameState, gcn.c_str(),
                g_inLobby ? "LOBBY" : "RUN");
            if (s_lastGSInst && !g_inLobby) {   // entered or reloaded a run instance
                DamageTracker::Reset();
                Log("[GS] run (re)start: damage reset\n");
            }
            s_lastGSInst = la.gameState;
        }
    }

    PollRunDamage(la);

    // GEAR PROBE (only in a run, where gear is equipped/active; capped). Hunts the known
    // +6/+2 affix magnitudes to locate where gear stores its stat bonuses.
    {
        static uint64_t s_ip = 0; static int s_ipN = 0;
        uint64_t nowMs = GetTickCount64();
        if (!g_inLobby && s_ipN < 5 && nowMs - s_ip > 3000 && (la.pawn || la.controller || la.state)) {
            s_ip = nowMs; s_ipN++;
            ProbeInventory(la);
        }
    }

    // STALENESS GUARD: even absent a detected pawn change, the cached set can be freed and
    // its memory reused. Drop it if its class pointer no longer matches what we bound.
    if (g_statsComponent && IsValidObject(g_statsComponent)) {
        void* cls = nullptr;
        if (!SafeReadU64(&g_statsComponent->ClassPrivate, (uint64_t*)&cls) ||
            (g_statsClass && cls != g_statsClass))
            g_statsComponent = nullptr;
    }

    if (!g_statsComponent || !IsValidObject(g_statsComponent)) {
        if (g_scanComplete) {
            g_statsComponent = nullptr;
            g_scanComplete = false;
            g_propsResolved = false;
            for (int i = 0; i < kNumStats; i++) {
                g_stats[i].found = false;
                g_stats[i].cachedOffset = -1;
                g_stats[i].value = 0.0f;
            }
        }
        return;
    }

    if (!g_propsResolved) {
        ResolvePropertyOffsets();
    }

    // ONE-SHOT diagnostic (was every 3s — that full-array scan + 190 log lines caused
    // hard frame stutter). Fire once after we've bound MY set, then never again.
    static bool s_probed = false;
    if (!s_probed && g_statsComponent) {
        s_probed = true;
        DumpAllAttributeSets();
    }

    for (int i = 0; i < kNumStats; i++) {
        if (g_stats[i].cachedOffset >= 0) {
            float val = 0.0f, base = 0.0f;
            // CURRENT (+12) reflects live GE modifiers/buffs; BASE (+8) is the unmodified
            // value. Their difference is the perk/augment/GE ("yellow") contribution.
            SafeReadFloat((uint8_t*)g_statsComponent + g_stats[i].cachedOffset + 8, &base);
            if (SafeReadFloat((uint8_t*)g_statsComponent + g_stats[i].cachedOffset + 12, &val)) {
                // Flash on change: stamp the time whenever the value actually moves.
                if (g_statPrevInit[i]) { float d = val - g_statPrev[i]; if (d > 0.0001f || d < -0.0001f) g_statFlashMs[i] = GetTickCount64(); }
                g_statPrev[i] = val; g_statPrevInit[i] = true;
                g_stats[i].value = val;
                g_statBase[i] = base;
                g_stats[i].found = true;
            }
        }
    }

    // EFFECTIVE BASE capture (drives the yellow split). Gated on the set being live (Max
    // Health > 1, index 0) so we never freeze a transient 0/default.
    //   LOBBY: no run pickups exist, so CurrentValue == raw+upgrades. Track it live every poll
    //          so buying perks updates the base and yellow stays 0.
    //   RUN:   freeze the base (carried in from the lobby). Only set-once here as a fallback if
    //          we somehow entered a run without a lobby pass. NOT reset on pawn/level changes,
    //          so accumulated pickups (yellow) persist across the run's levels.
    if (g_stats[0].found && g_stats[0].value > 1.0f) {
        for (int i = 0; i < kNumStats; i++) {
            if (!g_stats[i].found || g_stats[i].displayName[0] == '_') continue;
            if (g_inLobby)               { g_statBaseVal[i] = g_stats[i].value; g_statBaseSet[i] = true; }
            else if (!g_statBaseSet[i])  { g_statBaseVal[i] = g_stats[i].value; g_statBaseSet[i] = true; }
        }
    }

    // Diagnostic (re-armed per map): base(+8) vs cur(+12) vs effective base vs yellow.
    if (g_statbcDump) {
        bool anyReal = false;
        for (int i = 0; i < kNumStats; i++) if (g_stats[i].found) { anyReal = true; break; }
        if (anyReal && g_stats[0].value > 1.0f) {   // wait for the set to populate before dumping
            g_statbcDump = false;
            Log("[STATBC] inLobby=%d\n", (int)g_inLobby);
            for (int i = 0; i < kNumStats; i++) {
                if (!g_stats[i].found || g_stats[i].displayName[0] == '_') continue;
                Log("[STATBC] %-16s rawbase=%.3f cur=%.3f effbase=%.3f yellow=%.3f\n",
                    g_stats[i].displayName, g_statBase[i], g_stats[i].value,
                    g_statBaseSet[i] ? g_statBaseVal[i] : 0.0f,
                    g_statBaseSet[i] ? g_stats[i].value - g_statBaseVal[i] : 0.0f);
            }
        }
    }
}

// ── Public interface ────────────────────────────────────────────────────────

void Init() {
    g_initialized = true;
    Log("[STATS] Stats overlay initialized\n");
}

void Invalidate() {
    g_statsComponent = nullptr;
    g_scanComplete = false;
    g_propsResolved = false;
    for (int i = 0; i < kNumStats; i++) {
        g_stats[i].found = false;
        g_stats[i].value = 0.0f;
        g_stats[i].cachedOffset = -1;
    }
    Log("[STATS] Stats cache invalidated\n");
}

// HEAVY data polling — MUST run on the mod thread, NOT the render thread. It does
// ProcessEvent calls and full UObject-array scans; running it from Render() (which fires
// on the game's Present/render thread) hitches the game hard. Render() only draws the
// values this populates.
void Poll() {
    if (!g_initialized) return;

    uint64_t nowMs = GetTickCount64();

    // Fast damage read (~80ms) for responsive/accurate burst DPS — memory-only once bound.
    static uint64_t s_lastDmg = 0;
    if (nowMs - s_lastDmg >= 80) {
        s_lastDmg = nowMs;
        ReadDamageFast();
    }

    if (!g_scanComplete) {
        static uint64_t s_lastScan = 0;
        if (nowMs - s_lastScan > 1000) {
            s_lastScan = nowMs;
            ScanForStatsComponent();
        }
    }

    auto now = std::chrono::steady_clock::now();
    if (now - g_lastPoll >= kPollInterval) {
        g_lastPoll = now;
        PollStats();
    }
}

void Render() {
    if (!g_initialized) return;

    // Draw only — cached values are populated by Poll() on the mod thread.
    // ── ImGui rendering ─────────────────────────────────────────────────
    // Center-right of the screen, below the damage window (which sits just above it).
    ImGuiIO& io = ImGui::GetIO();
    float scrW = io.DisplaySize.x, scrH = io.DisplaySize.y;
    float windowWidth = 360.0f;
    ImGui::SetNextWindowPos(ImVec2(scrW - windowWidth - 20.0f, scrH * 0.30f + 200.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::SetNextWindowSize(ImVec2(windowWidth, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoNav;

    if (!ImGui::Begin("Player Stats", nullptr, flags)) {
        ImGui::End();
        return;
    }

    if (!g_statsComponent) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Waiting for match...");
        g_scanComplete = false;
        ImGui::End();
        return;
    }

    const ImVec4 kYellow(1.00f, 0.85f, 0.20f, 1.0f);   // perk / augment / item (via GE)
    const ImVec4 kBlue  (0.40f, 0.70f, 1.00f, 1.0f);   // armor / gear (pending probe)
    const ImVec4 kGreen (0.35f, 0.92f, 0.45f, 1.0f);   // total
    const ImVec4 kFlash (0.75f, 1.00f, 0.80f, 1.0f);   // subtle brighten on change (fades to green)
    uint64_t nowFlash = GetTickCount64();

    bool anyFound = false;
    for (int i = 0; i < kNumStats; i++) {
        if (!g_stats[i].found) continue;
        if (g_stats[i].displayName[0] == '_') continue;   // hidden (e.g. __DamageDealt)
        anyFound = true;

        float val    = g_stats[i].value;
        // yellow = in-run gear/item pickups = CurrentValue - effective base, where the
        // effective base (raw + character upgrades) was captured in the lobby. 0 until captured.
        float yellow = g_statBaseSet[i] ? (val - g_statBaseVal[i]) : 0.0f;

        ImGui::TextUnformatted(g_stats[i].displayName);
        ImGui::SameLine(windowWidth * 0.52f);

        // ── total ──
        char tb[32];
        switch (g_stats[i].format) {
        case StatDef::Absolute:   snprintf(tb, sizeof(tb), "%.0f", val); break;
        case StatDef::Multiplier: snprintf(tb, sizeof(tb), "x%.2f", val); break;
        case StatDef::Modifier:   snprintf(tb, sizeof(tb), "%+.1f%%", val * 100.0f); break;
        case StatDef::Chance:     snprintf(tb, sizeof(tb), "%.1f%%", val * 100.0f); break;
        }
        // Green total; briefly lightened toward kFlash right after a change, easing back.
        ImVec4 totalCol = kGreen;
        if (g_statFlashMs[i]) {
            uint64_t dt = nowFlash - g_statFlashMs[i];
            if (dt < 700) {
                float t = (float)dt / 700.0f;   // 0 = just changed .. 1 = settled
                totalCol = ImVec4(kFlash.x + (kGreen.x - kFlash.x) * t,
                                  kFlash.y + (kGreen.y - kFlash.y) * t,
                                  kFlash.z + (kGreen.z - kFlash.z) * t, 1.0f);
            }
        }
        ImGui::TextColored(totalCol, "%s", tb);

        // ── yellow contribution (Current - Base) ──
        bool showY = false; char yb[32] = {};
        switch (g_stats[i].format) {
        case StatDef::Absolute:   if (yellow >  0.5f  || yellow < -0.5f)  { snprintf(yb, sizeof(yb), "(+%.0f)", yellow);        showY = true; } break;
        case StatDef::Multiplier: if (yellow >  0.005f|| yellow < -0.005f){ snprintf(yb, sizeof(yb), "(%+.2f)", yellow);        showY = true; } break;
        case StatDef::Modifier:
        case StatDef::Chance:      if (yellow > 0.0005f|| yellow < -0.0005f){ snprintf(yb, sizeof(yb), "(%+.1f%%)", yellow*100.0f); showY = true; } break;
        }
        if (showY) { ImGui::SameLine(); ImGui::TextColored(kYellow, "%s", yb); }
    }

    if (!anyFound) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Resolving stats...");
        g_propsResolved = false;
    }

    // ── color legend ──
    ImGui::Separator();
    ImGui::TextColored(kGreen, "total"); ImGui::SameLine();
    ImGui::TextColored(kBlue, "gear"); ImGui::SameLine();
    ImGui::TextColored(kYellow, "perk/aug/item");
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(gear breakdown pending)");

    ImGui::End();
}

} // namespace StatsOverlay
