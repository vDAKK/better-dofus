#include "jobs/craft-list.hpp"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace craftList {
namespace {
using Json = nlohmann::json;
// No quantity of the game comes close; this keeps a product from overflowing.
constexpr int64_t MaxQuantity = std::numeric_limits<int64_t>::max() / 4;

int64_t Times(int64_t quantity, int64_t factor) {
    if (quantity <= 0 || factor <= 0) return 0;
    return quantity > MaxQuantity / factor ? MaxQuantity : quantity * factor;
}
int64_t Plus(int64_t left, int64_t right) { return std::min(MaxQuantity, left + right); }

struct Walk {
    const RecipeOf& recipeOf;
    const OwnedOf& ownedOf;
    bool subRecipes;
    std::unordered_map<int, int64_t> stock;
    std::map<int, Need> buy, craft;
    std::vector<int> path;
    int visits = 0;
    bool truncated = false;

    void Demand(int item, int64_t quantity) {
        if (quantity <= 0 || item <= 0) return;
        if (++visits > MaxVisits) { truncated = true; return; }
        auto available = stock.find(item);
        if (available == stock.end()) available = stock.emplace(item, std::max<int64_t>(0, ownedOf(item))).first;
        const auto taken = std::min(available->second, quantity);
        available->second -= taken;
        const auto missing = quantity - taken;
        const std::vector<Part>* parts = nullptr;
        if (subRecipes && std::find(path.begin(), path.end(), item) == path.end()) {
            parts = recipeOf(item);
            if (parts && static_cast<int>(path.size()) >= MaxDepth) { parts = nullptr; truncated = true; }
        }
        auto& need = (parts && !parts->empty() ? craft : buy)[item];
        need.item = item;
        need.needed = Plus(need.needed, quantity);
        need.covered = Plus(need.covered, taken);
        need.missing = Plus(need.missing, missing);
        if (!parts || parts->empty() || missing == 0) return;
        path.push_back(item);
        for (const auto& part : *parts) Demand(part.item, Times(missing, part.quantity));
        path.pop_back();
    }
};
std::vector<Need> Sorted(const std::map<int, Need>& needs) {
    std::vector<Need> result;
    for (const auto& [item, need] : needs) result.push_back(need);
    std::stable_sort(result.begin(), result.end(), [](const Need& left, const Need& right) { return (left.missing > 0) > (right.missing > 0); });
    return result;
}
}

int Add(List& list, int item, int count) {
    if (item <= 0 || count <= 0) return 0;
    for (auto& entry : list.entries) {
        if (entry.item != item) continue;
        entry.count = std::min(MaxCount, entry.count + count);
        return entry.count;
    }
    if (list.entries.size() >= MaxEntries) return 0;
    const int bounded = std::min(MaxCount, count);
    list.entries.push_back({item, bounded, 0});
    return bounded;
}
void SetCount(List& list, int item, int count) {
    if (count <= 0) { Remove(list, item); return; }
    for (auto& entry : list.entries) {
        if (entry.item != item) continue;
        entry.count = std::min(MaxCount, count);
        entry.made = std::min(entry.made, entry.count);
    }
}
void Remove(List& list, int item) {
    list.entries.erase(std::remove_if(list.entries.begin(), list.entries.end(), [&](const Entry& entry) { return entry.item == item; }), list.entries.end());
}
int Remaining(const Entry& entry) { return std::max(0, entry.count - entry.made); }

Plan Compute(const List& list, const RecipeOf& recipeOf, const OwnedOf& ownedOf) {
    Walk walk{recipeOf, ownedOf, list.subRecipes};
    for (const auto& entry : list.entries) {
        const auto parts = recipeOf(entry.item);
        if (!parts) continue;
        walk.path.assign(1, entry.item);
        for (const auto& part : *parts) walk.Demand(part.item, Times(Remaining(entry), part.quantity));
    }
    return {Sorted(walk.buy), Sorted(walk.craft), walk.truncated};
}

bool Observe(List& list, Tracker& tracker, const std::function<Reading(int)>& read) {
    bool changed = false;
    std::map<int, Reading> next;
    for (auto& entry : list.entries) {
        const auto reading = read(entry.item);
        const auto last = tracker.seen.find(entry.item);
        if (!reading.bagKnown) {
            if (last != tracker.seen.end()) next.insert(*last);
            continue;
        }
        next[entry.item] = reading;
        if (last == tracker.seen.end()) continue;
        const auto& before = last->second;
        auto gain = reading.bag - before.bag;
        if (reading.bankKnown && before.bankKnown) gain += reading.bank - before.bank;
        if (reading.havenBagKnown && before.havenBagKnown) gain += reading.havenBag - before.havenBag;
        if (gain <= 0 || entry.made >= entry.count) continue;
        entry.made = static_cast<int>(std::min<int64_t>(entry.count, entry.made + gain));
        changed = true;
    }
    tracker.seen = std::move(next);
    return changed;
}

std::string Serialize(const List& list) {
    Json entries = Json::array();
    for (const auto& entry : list.entries) entries.push_back({{"item", entry.item}, {"count", entry.count}, {"made", entry.made}});
    return Json{{"version", 1}, {"subRecipes", list.subRecipes}, {"entries", entries}}.dump(1);
}
List Parse(const std::string& text) {
    const auto json = Json::parse(text);
    if (!json.is_object() || !json.at("entries").is_array()) throw std::runtime_error("Craft list format");
    List list;
    list.subRecipes = json.value("subRecipes", false);
    for (const auto& value : json.at("entries")) {
        const int item = value.at("item").get<int>(), count = value.at("count").get<int>();
        const int made = value.value("made", 0);
        if (item <= 0 || count <= 0 || count > MaxCount || list.entries.size() >= MaxEntries) continue;
        if (std::any_of(list.entries.begin(), list.entries.end(), [&](const Entry& entry) { return entry.item == item; })) continue;
        list.entries.push_back({item, count, std::clamp(made, 0, count)});
    }
    return list;
}
std::filesystem::path DataPath(const std::filesystem::path& settingsPath) {
    return settingsPath.parent_path() / L"BetterDofus-craft-list.json";
}
List Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream text;
    text << file.rdbuf();
    file.close();
    try { return Parse(text.str()); }
    catch (const std::exception&) {
        auto aside = path; aside += L".corrupt";
        MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        return {};
    }
}
void Save(const std::filesystem::path& path, const List& list) {
    static std::atomic<unsigned> counter{0};
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << Serialize(list);
        file.flush();
        if (!file) throw std::runtime_error("Cannot write the craft list");
    }
    // Another account's process may be replacing the same file for a moment.
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        Sleep(20);
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw std::runtime_error("Cannot publish the craft list");
}
}
