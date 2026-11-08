#include "ui/monster-target-policy.hpp"
#include <iostream>
#include <stdexcept>

using namespace monsterTarget;

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
    try {
        Check(Valid({}) && Valid({31, 494}), "Sorted lists are valid");
        Check(!Valid({494, 31}) && !Valid({31, 31}) && !Valid({0}) && !Valid({MaxMonsterId + 1}), "Unsorted, repeated or out of range lists are not");

        auto targets = Toggle({}, 494, true);
        targets = Toggle(targets, 31, true);
        Check(targets == std::vector<int>{31, 494}, "Targets stay sorted");
        Check(Toggle(targets, 31, true) == targets, "Targeting twice changes nothing");
        Check(Toggle(targets, 31, false) == std::vector<int>{494}, "A target is removed");
        Check(Toggle(targets, 77, false) == targets, "Removing a monster not targeted changes nothing");
        Check(Toggle(targets, 0, true) == targets && Toggle(targets, -3, true) == targets, "Bad ids are ignored");
        std::vector<int> full;
        for (int id = 1; id <= static_cast<int>(MaxTargets); ++id) full.push_back(id);
        Check(Toggle(full, 9999, true) == full, "A full list stays as it is");
        Check(Valid(Toggle(full, 5, false)), "A full list still loses a target");

        Check(Targeted({31, 494}, {12, 494, 12}), "Another monster of the group counts");
        Check(Targeted({31}, {31}), "The main monster counts");
        Check(!Targeted({31}, {12, 13}) && !Targeted({}, {31}) && !Targeted({31}, {}), "No target in the group");

        const auto badge = Badge(100.f, 50.f);
        Check(badge.x == 100.f - BadgeSize / 2 && badge.y == 50.f - BadgeSize - BadgeGap && badge.width == BadgeSize && badge.height == BadgeSize,
            "The badge is centred above the sprites");
        Check(Finite(badge) && !Finite(Badge(NAN, 1.f)), "Projection failures are caught");
        Check(Bob(0) == 0.f && std::fabs(Bob(BobPeriodMs / 2) - BobPixels) < 1e-4f && Bob(BobPeriodMs) == 0.f, "The badge rises and falls back");
        for (uint64_t t = 0; t < 5000; t += 37) Check(Bob(t) >= 0.f && Bob(t) <= BobPixels + 1e-4f, "The rise stays bounded");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
