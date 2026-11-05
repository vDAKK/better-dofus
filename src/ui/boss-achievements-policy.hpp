#pragma once
#include <algorithm>
#include <string>
#include <vector>

// A boss's achievements: which ones, in which order, in which words. Pure: the data and
// what is finished come from the game (boss-achievements.cpp).
namespace bossAchievements {
inline constexpr size_t MaxItems = 64;
struct Item { int id = 0, points = 0, level = 0; std::string name, description; bool done = false; };
struct Sorted { std::vector<Item> todo, done; };

// The boss's lists then its dungeon's: each achievement once, in that order.
inline std::vector<int> Merge(const std::vector<std::vector<int>>& lists) {
    std::vector<int> result;
    for (const auto& list : lists)
        for (const int id : list)
            if (id > 0 && result.size() < MaxItems && std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);
    return result;
}
// Lowest level first; equal levels keep the game's order.
inline Sorted Sort(std::vector<Item> items) {
    std::stable_sort(items.begin(), items.end(), [](const Item& left, const Item& right) { return left.level < right.level; });
    Sorted sorted;
    for (auto& item : items) (item.done ? sorted.done : sorted.todo).push_back(std::move(item));
    return sorted;
}
// The game's links, {monster,123::Name}, read as their name.
inline std::string Plain(const std::string& text) {
    std::string result;
    for (size_t i = 0; i < text.size();) {
        const size_t close = text[i] == '{' ? text.find('}', i) : std::string::npos;
        const size_t split = close == std::string::npos ? std::string::npos : text.find("::", i);
        if (split != std::string::npos && split < close) {
            result += text.substr(split + 2, close - split - 2);
            i = close + 1;
        } else result += text[i++];
    }
    return result;
}
// known: the game has told which achievements are finished.
inline std::string MenuLabel(const std::string& boss, const Sorted& sorted, bool known) {
    std::string label = "Succès : " + boss;
    if (!known) return label;
    if (sorted.todo.empty()) return label + " (tous faits)";
    return label + " (" + std::to_string(sorted.todo.size()) + " à faire)";
}
}
