#include "damage_tracker.h"
#include <Windows.h>
#include <imgui.h>
#include <mutex>
#include <deque>
#include <cstdio>

namespace DamageTracker {

struct Hit { uint64_t t; float dmg; };

static std::mutex        g_mtx;
static std::deque<Hit>   g_hits;         // recent hits, pruned to the 10s window
static double            g_total = 0.0;  // running total for the run
static double            g_lastCumulative = -1.0;  // last lifetime value seen (for deltas)
static double            g_baseline       = -1.0;  // lifetime value at run start; run total = now - baseline
static bool              g_resync         = false; // next sample resyncs without counting the gap as a burst

// Peak DPS reached in each window (indices: 0=1s,1=2s,2=5s,3=10s).
static double            g_peak[4] = {0, 0, 0, 0};
static const double      kWinSecs[4] = { 1.0, 2.0, 5.0, 10.0 };
static const uint64_t    kWinMs[4]   = { 1000, 2000, 5000, 10000 };

static void FmtNum(double v, char* out, size_t n) {
    if (v >= 1.0e6)      snprintf(out, n, "%.2fM", v / 1.0e6);
    else if (v >= 1.0e3) snprintf(out, n, "%.1fK", v / 1.0e3);
    else                 snprintf(out, n, "%.0f", v);
}

void AddDamage(float amount) {
    if (!(amount > 0.0f)) return;
    uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_mtx);
    g_hits.push_back({now, amount});
    g_total += (double)amount;
    while (!g_hits.empty() && now - g_hits.front().t > 10000)
        g_hits.pop_front();
}

// Fed with the game's LIFETIME damage counter (ValPlayerStatsComponent). The per-run
// total is the increase since run start, so we baseline the counter and display
// (now - baseline). DPS windows come from the per-poll positive deltas. Reset() (called
// on a new life/level) clears the baseline so it re-snapshots; a counter DROP also
// re-baselines defensively.
void SetCumulative(double lifetime) {
    if (lifetime < 0.0) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    uint64_t now = GetTickCount64();

    if (g_baseline < 0.0 || lifetime < g_baseline - 0.5) {  // run start / counter reset
        g_baseline = lifetime;
        g_lastCumulative = lifetime;
        g_total = 0.0;
        g_hits.clear();
        for (int i = 0; i < 4; i++) g_peak[i] = 0.0;
        return;
    }

    if (g_resync) {   // after a component re-bind: resync the cursor, don't count the gap
        g_resync = false;
        g_lastCumulative = lifetime;
        g_total = lifetime - g_baseline;
        return;
    }

    double delta = lifetime - g_lastCumulative;
    g_lastCumulative = lifetime;
    g_total = lifetime - g_baseline;           // damage THIS run
    if (delta > 0.0) {
        g_hits.push_back({now, (float)delta});
        while (!g_hits.empty() && now - g_hits.front().t > 10000)
            g_hits.pop_front();
    }
}

void Reset() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_hits.clear();
    g_total = 0.0;
    g_lastCumulative = -1.0;
    g_baseline = -1.0;   // re-baseline on next sample (new life/level)
    for (int i = 0; i < 4; i++) g_peak[i] = 0.0;
}

// Call after the damage source re-binds (level transition): the next SetCumulative
// resyncs the delta cursor instead of counting the whole gap as one instantaneous burst
// (which would spike the Peak). Keeps the run total intact.
void MarkDiscontinuity() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_resync = true;
}

static double SumWindow(uint64_t now, uint64_t windowMs) {
    double s = 0.0;
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
        if (now - it->t > windowMs) break;
        s += it->dmg;
    }
    return s;
}

void Render() {
    uint64_t now = GetTickCount64();
    double total, dps[4], peak[4];
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        while (!g_hits.empty() && now - g_hits.front().t > 10000)
            g_hits.pop_front();
        for (int i = 0; i < 4; i++) {
            double windowSum = SumWindow(now, kWinMs[i]);   // total damage in the last N seconds
            dps[i] = windowSum / kWinSecs[i];               // DPS column = per-second RATE
            if (windowSum > g_peak[i]) g_peak[i] = windowSum;  // Peak column = most TOTAL damage in any N-sec window
            peak[i] = g_peak[i];
        }
        total = g_total;
    }

    // Center-right of the screen (stats window sits just below this).
    ImGuiIO& dio = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(dio.DisplaySize.x - 250.0f, dio.DisplaySize.y * 0.30f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
                           | ImGuiWindowFlags_NoTitleBar;

    if (ImGui::Begin("##DamageTracker", nullptr, flags)) {
        char buf[32];
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "DAMAGE");
        ImGui::Separator();

        FmtNum(total, buf, sizeof(buf));
        ImGui::Text("Total"); ImGui::SameLine(96); ImGui::Text("%s", buf);

        // Header row for the DPS / Peak columns.
        ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1), "       DPS");
        ImGui::SameLine(160); ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1), "Peak");

        const char* labels[4] = { "1s", "2s", "5s", "10s" };
        for (int i = 0; i < 4; i++) {
            ImGui::Text("%s", labels[i]);
            FmtNum(dps[i],  buf, sizeof(buf)); ImGui::SameLine(96);  ImGui::Text("%s", buf);
            FmtNum(peak[i], buf, sizeof(buf)); ImGui::SameLine(160); ImGui::TextColored(ImVec4(1.0f,0.5f,0.3f,1), "%s", buf);
        }
    }
    ImGui::End();
}

} // namespace DamageTracker
