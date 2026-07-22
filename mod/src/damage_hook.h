#pragma once

// Read-only native hook that feeds the damage tracker. Hooks UValActor::OnDealtDamage
// (RVA 0x61938C0), which carries the real per-hit TotalDamage (crit/weakpoint applied).
// No injection — purely observational.
namespace DamageHook {

void Init();
void Shutdown();

} // namespace DamageHook
