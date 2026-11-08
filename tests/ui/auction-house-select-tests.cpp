#include "ui/auction-house-select-policy.hpp"
#include <iostream>
#include <stdexcept>

using namespace auctionHouseSelect;

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
    try {
        const std::vector<Key> lots{{10, 0}, {20, 1}, {30, 2}, {40, 3}, {50, 4}};
        Check(Find(lots, {30, 2}) == 2u, "A lot is found by its item and index");
        Check(!Find(lots, {30, 3}), "Another index is another lot");
        Check(!Find(lots, {60, 2}), "Another item is another lot");
        Check(!Find(lots, {}), "No anchor");
        Check(!Find({}, {10, 0}), "An empty list");
        const std::vector<Key> twice{{10, 0}, {10, 0}};
        Check(Find(twice, {10, 0}) == 0u, "The first match");

        Check(Between(1, 4) == std::vector<size_t>{1, 2, 3}, "Down the list, without the clicked lot");
        Check(Between(4, 1) == std::vector<size_t>{2, 3, 4}, "Up the list, without the clicked lot");
        Check(Between(2, 2).empty(), "The anchor clicked again");
        Check(Between(0, 1) == std::vector<size_t>{0}, "Next lot");
        Check(Between(0, 467).size() == 467, "A long list");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
