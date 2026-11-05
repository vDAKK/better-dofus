#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The game's own recipes, average prices and item quantities. None of this asks the
// network. Each answer is « unknown » rather than an error when the game cannot say.
namespace recipeNative {
struct Ingredient { int id = 0, quantity = 0; std::string name; };
struct Recipe { int result = 0, level = 0; std::vector<Ingredient> ingredients; };
// The bank is only known once the game holds its content.
struct Stock { int64_t bag = 0, bank = 0; bool bagKnown = false, bankKnown = false; };

void Install();
std::optional<Recipe> Find(int resultItem);
int64_t AveragePrice(int item); // 0: unknown
Stock Owned(int item);
}
