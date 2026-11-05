#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// What a recipe costs and what is left to gather. Pure: the prices and quantities
// come from the game (recipe-native.hpp).
namespace recipeCost {
struct Line { int quantity = 0; int64_t unitPrice = 0; };
struct Total { int64_t cost = 0; int unpriced = 0; };
// A price of zero or less is unknown: the game has no average price for that item.
inline Total Sum(const std::vector<Line>& lines, int64_t count = 1) {
    Total total;
    for (const auto& line : lines) {
        if (line.unitPrice > 0) total.cost += line.unitPrice * line.quantity * count;
        else ++total.unpriced;
    }
    return total;
}
// 1234567 -> "1 234 567 K"
inline std::string Kamas(int64_t value) {
    auto digits = std::to_string(value < 0 ? -value : value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), " ");
    return (value < 0 ? "-" : "") + digits + " K";
}
inline int64_t Missing(int64_t needed, int64_t owned) { return std::max<int64_t>(0, needed - owned); }
// What selling at the item's average price leaves. Known only if every price is.
struct Margin { bool known = false; int64_t value = 0; };
inline Margin Estimate(const Total& total, int64_t averagePrice) {
    if (averagePrice <= 0 || total.cost <= 0 || total.unpriced) return {};
    return {true, averagePrice - total.cost};
}
}
