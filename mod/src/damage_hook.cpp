#include "damage_hook.h"
#include "damage_tracker.h"
#include "ue_engine.h"
#include <Windows.h>
#include <MinHook.h>
#include <cstring>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace DamageHook {

static bool SafeRead(const void* addr, void* buf, size_t len) {
    __try {
        memcpy(buf, addr, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── OnDealtDamage: per outgoing damage instance, carries FValDamageDetail with the
// real TotalDamage (crit/weakpoint applied). RVA 0x61938C0. arg2 = FValDamageDetail*.
// +0x118 is the standout per-hit TotalDamage. Read-only — just feeds the tracker.
static const uintptr_t ONDEALTDMG_RVA = 0x61938C0;
typedef void* (__fastcall* OnDealtDmgFn)(void*, void*, void*);
static OnDealtDmgFn g_origOnDealtDmg = nullptr;
static uintptr_t    g_hookAddr = 0;
static bool         g_hooked   = false;

static void* __fastcall HookedOnDealtDmg(void* thisPtr, void* detail, void* a3) {
    if (detail) {
        float total = 0.0f;
        SafeRead((uint8_t*)detail + 0x118, &total, 4);
        if (total > 0.0f && total < 1.0e7f)
            DamageTracker::AddDamage(total);
    }
    return g_origOnDealtDmg(thisPtr, detail, a3);
}

// DISABLED. The OnDealtDamage hook undercounted badly (~41% of true damage: it saw only
// hits routed through this path, missing DoT/AoE/proc/pet damage, and +0x118 was a
// pre-crit/weakpoint value). Superseded by reading the game's AUTHORITATIVE per-run total
// from ValPlayerStatsComponent (polled in stats_overlay.cpp), which sums every damage
// application, matches the end screen exactly, and auto-clears on run restart. Left as a
// no-op so nothing double-feeds the tracker. MinHook lifetime is owned by the present hook.
void Init() {
    (void)ONDEALTDMG_RVA; (void)&HookedOnDealtDmg; (void)g_origOnDealtDmg;
    (void)g_hookAddr; (void)g_hooked;
    Log("[DMGHOOK] disabled — damage read from ValPlayerStatsComponent instead\n");
}

void Shutdown() {}

} // namespace DamageHook
