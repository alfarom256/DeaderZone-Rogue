#pragma once

// Persistent damage tracker overlay (top-left). Total damage, burst DPS over
// 1/2/5/10s windows, and the PEAK DPS reached in each window.
namespace DamageTracker {

// Feed a discrete damage event (kept for compatibility; not the primary source).
void AddDamage(float amount);

// Primary source: the game's cumulative "DamageDealt" stat. Called each poll with
// the running total; the tracker derives per-interval damage from the deltas.
void SetCumulative(double cumulativeDamage);

// Call after the damage source re-binds (level transition) so the gap isn't counted as
// one instantaneous burst (which would spike Peak). Run total is preserved.
void MarkDiscontinuity();

// Called each frame from the render loop / present hook.
void Render();

// Clear all accumulated damage + peaks (e.g. on new run).
void Reset();

} // namespace DamageTracker
