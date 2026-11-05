#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

// The crafts the player plans, and what they still take. Pure but for the file: the
// recipes and quantities come from the game (recipe-native.hpp) through callbacks.
namespace craftList {
inline constexpr int MaxCount = 9999;
inline constexpr size_t MaxEntries = 40;
inline constexpr int MaxDepth = 8;
inline constexpr int MaxVisits = 20000;

// made: the items obtained since the craft was added. They count, or using the
// ingredients would raise the need instead of lowering it.
struct Entry { int item = 0, count = 0, made = 0; };
struct List {
    std::vector<Entry> entries;
    bool subRecipes = false;
};
// Returns the entry's new count, 0 when the list is full.
int Add(List& list, int item, int count = 1);
// A count of zero or less removes the craft.
void SetCount(List& list, int item, int count);
void Remove(List& list, int item);
int Remaining(const Entry& entry);

struct Part { int item = 0, quantity = 0; };
// nullptr: the item has no recipe.
using RecipeOf = std::function<const std::vector<Part>*(int item)>;
using OwnedOf = std::function<int64_t(int item)>;
// needed: everything the crafts use of it. covered: what the stock pays for.
// missing: what is left, to buy or to craft first.
struct Need { int item = 0; int64_t needed = 0, covered = 0, missing = 0; };
struct Plan {
    std::vector<Need> buy, craft; // Unfinished first, then by item.
    bool truncated = false;
};
// Each unit of stock serves once, whichever craft asks first. With the sub-recipes,
// what is missing of a craftable ingredient asks for its own ingredients instead of
// being bought; a recipe needing itself is bought.
Plan Compute(const List& list, const RecipeOf& recipeOf, const OwnedOf& ownedOf);

// Counts what the crafts' results gain between two looks, over the places known both
// times: the bank just read is not a gain, and neither is a withdrawal from it.
struct Reading { int64_t bag = 0, bank = 0, havenBag = 0; bool bagKnown = false, bankKnown = false, havenBagKnown = false; };
struct Tracker { std::map<int, Reading> seen; };
bool Observe(List& list, Tracker& tracker, const std::function<Reading(int item)>& read);

std::string Serialize(const List& list);
// Throws on a corrupt text. Out of bounds entries are dropped.
List Parse(const std::string& text);
std::filesystem::path DataPath(const std::filesystem::path& settingsPath);
// An absent file is an empty list; a corrupt one is kept aside as .corrupt.
List Load(const std::filesystem::path& path);
void Save(const std::filesystem::path& path, const List& list);
}
