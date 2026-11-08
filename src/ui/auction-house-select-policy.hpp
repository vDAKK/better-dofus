#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// « Sélection avec Maj » à l'hôtel de vente (2.14.0): which of the player's lots a
// Shift click adds to the selection. Pure. A lot is known by its item and its index,
// both read from the game's list, so the anchor survives a scroll but not a new list.
namespace auctionHouseSelect {
struct Key {
    uintptr_t item = 0;
    int index = -1;
    bool operator==(const Key& other) const { return item == other.item && index == other.index; }
};

inline std::optional<size_t> Find(const std::vector<Key>& lots, const Key& key) {
    if (!key.item) return std::nullopt;
    for (size_t i = 0; i < lots.size(); ++i)
        if (lots[i] == key) return i;
    return std::nullopt;
}

// Every lot from the anchor to the clicked one, both included, in the list's order
// and in either direction, except the clicked one: the game selects that one itself.
inline std::vector<size_t> Between(size_t anchor, size_t clicked) {
    std::vector<size_t> result;
    const auto first = anchor < clicked ? anchor : clicked, last = anchor < clicked ? clicked : anchor;
    for (auto i = first; i <= last; ++i)
        if (i != clicked) result.push_back(i);
    return result;
}
}
