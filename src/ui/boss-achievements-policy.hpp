#pragma once
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

// A monster's achievements: which ones, in which order, in which words. Pure: the data
// and what is finished come from the game (boss-achievements.cpp).
namespace bossAchievements {
inline constexpr size_t MaxItems = 64;
struct Item { int id = 0, points = 0, level = 0, icon = 0; std::string name, description; bool done = false; };
struct Sorted { std::vector<Item> todo, done; };

// The monsters' lists then the dungeon's: each achievement once, in that order.
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
// A group's monsters with achievements, bosses first: the first one, and how many more.
inline std::string Title(const std::vector<std::string>& monsters) {
    if (monsters.empty()) return "";
    if (monsters.size() == 1) return monsters.front();
    return monsters.front() + " et " + std::to_string(monsters.size() - 1) + (monsters.size() == 2 ? " autre" : " autres");
}
// An objective, and the monster its criterion asks for: (Ef>4818,0) is monster 4818.
struct Objective { int id = 0, monster = 0; std::string name; };
inline int MonsterOf(const std::string& criterion) {
    const auto start = criterion.find("Ef>");
    if (start == std::string::npos) return 0;
    int monster = 0;
    for (size_t i = start + 3; i < criterion.size() && std::isdigit(static_cast<unsigned char>(criterion[i])); ++i) {
        if (monster > 100000000) return 0;
        monster = monster * 10 + (criterion[i] - '0');
    }
    return monster;
}
// Whether this fight advances the achievement: one of its enemies is a monster still
// missing. An achievement that names no monster stays: the game linked it to them.
inline bool Advances(const std::vector<Objective>& objectives, const std::set<int>& finished, const std::vector<int>& enemies) {
    bool named = false;
    for (const auto& objective : objectives) {
        if (!objective.monster) continue;
        named = true;
        if (!finished.count(objective.id) && std::find(enemies.begin(), enemies.end(), objective.monster) != enemies.end()) return true;
    }
    return !named;
}
// How far an achievement's monsters are: its other objectives do not count.
struct Progress { int done = 0, total = 0; };
inline Progress Measure(const std::vector<Objective>& objectives, const std::set<int>& finished) {
    Progress progress;
    for (const auto& objective : objectives) {
        if (!objective.monster) continue;
        ++progress.total;
        if (finished.count(objective.id)) ++progress.done;
    }
    return progress;
}
inline int Percent(const Progress& progress) { return progress.total ? progress.done * 100 / progress.total : 0; }
inline std::string Summary(const Progress& progress) {
    const int left = progress.total - progress.done;
    return std::to_string(Percent(progress)) + " % validé, " + std::to_string(left) + (left == 1 ? " monstre restant" : " monstres restants");
}
// A monster of the group, in a tooltip's list.
enum class Mark { None, Missing, Done };
inline Mark MarkOf(int monster, const std::vector<Objective>& objectives, const std::set<int>& finished) {
    Mark mark = Mark::None;
    for (const auto& objective : objectives) {
        if (objective.monster != monster) continue;
        if (!finished.count(objective.id)) return Mark::Missing;
        mark = Mark::Done;
    }
    return mark;
}
// A quest's objective speaks of this monster: one of its parameters is its id, and its
// text names it. The id alone could be an item's.
inline bool ObjectiveOnMonster(const std::vector<int>& parameters, int monster, const std::string& foldedText, const std::string& foldedName) {
    return !foldedName.empty() && std::find(parameters.begin(), parameters.end(), monster) != parameters.end() && foldedText.find(foldedName) != std::string::npos;
}
// known: the game's answer on this achievement has come.
inline std::string ObjectiveLine(const std::string& name, bool known, bool done) {
    return (known ? (done ? "[x] " : "[ ] ") : "- ") + name;
}
// What an achievement's icon says when hovered in the fight's panel.
inline std::string Tip(const Item& item, const std::vector<std::string>& objectives) {
    std::string tip = item.name + "\nSuccès à faire : " + std::to_string(item.points) + " points";
    if (!item.description.empty()) tip += "\n" + item.description;
    for (const auto& objective : objectives) tip += "\n" + objective;
    return tip;
}
// known: the game has told which achievements are finished.
inline std::string MenuLabel(const std::string& monster, const Sorted& sorted, bool known) {
    std::string label = "Succès : " + monster;
    if (!known) return label;
    if (sorted.todo.empty()) return label + " (tous faits)";
    return label + " (" + std::to_string(sorted.todo.size()) + " à faire)";
}
}
