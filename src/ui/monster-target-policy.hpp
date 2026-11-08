#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// « Monstres ciblés » (2.15.0): the monsters the player targets from the bestiary, and
// where their badge goes above a group. Pure.
namespace monsterTarget {
inline constexpr size_t MaxTargets = 128;
inline constexpr int MaxMonsterId = 1000000;

// The saved list: sorted, without duplicates, bounded, as Settings keeps it.
inline bool Valid(const std::vector<int>& targets) {
    if (targets.size() > MaxTargets) return false;
    int previous = 0;
    for (int id : targets) {
        if (id <= previous || id > MaxMonsterId) return false;
        previous = id;
    }
    return true;
}
inline bool Contains(const std::vector<int>& targets, int id) {
    return std::binary_search(targets.begin(), targets.end(), id);
}
// The list with this monster targeted or not. A full list stays as it is.
inline std::vector<int> Toggle(std::vector<int> targets, int id, bool on) {
    if (id <= 0 || id > MaxMonsterId) return targets;
    const auto where = std::lower_bound(targets.begin(), targets.end(), id);
    const bool present = where != targets.end() && *where == id;
    if (on && !present && targets.size() < MaxTargets) targets.insert(where, id);
    else if (!on && present) targets.erase(where);
    return targets;
}
// A group counts when one of its monsters, the main one or another, is targeted.
inline bool Targeted(const std::vector<int>& targets, const std::vector<int>& monsters) {
    return std::any_of(monsters.begin(), monsters.end(), [&](int id) { return Contains(targets, id); });
}

// The badge in the HUD's coordinates (origin at the top left, y down): centred on the
// top of the group's sprites, a little above it.
struct Rect { float x = 0, y = 0, width = 0, height = 0; };
inline constexpr float BadgeSize = 40.f, BadgeGap = 4.f, BobPixels = 3.f;
inline constexpr uint64_t BobPeriodMs = 1200;
// A slow rise and fall that catches the eye, 0 to BobPixels and back.
inline float Bob(uint64_t nowMs) {
    const float phase = static_cast<float>(nowMs % BobPeriodMs) / BobPeriodMs;
    return BobPixels * (1.f - std::cos(phase * 6.2831853f)) / 2.f;
}
inline bool Finite(Rect r) { return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height); }
inline Rect Badge(float centerX, float topY, float size = BadgeSize) {
    return {centerX - size / 2, topY - size - BadgeGap, size, size};
}
}
