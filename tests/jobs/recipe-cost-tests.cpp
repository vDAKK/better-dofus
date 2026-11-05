#include "jobs/recipe-cost.hpp"
#include <iostream>
#include <stdexcept>

using namespace recipeCost;

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
    try {
        Check(Kamas(0) == "0 K" && Kamas(999) == "999 K" && Kamas(1000) == "1 000 K" && Kamas(1234567) == "1 234 567 K", "Kamas grouping");
        Check(Kamas(-1234) == "-1 234 K", "Negative kamas");
        const auto simple = Sum({{2, 100}, {3, 10}});
        Check(simple.cost == 230 && simple.unpriced == 0, "Quantity times price");
        const auto many = Sum({{2, 100}, {3, 10}}, 5);
        Check(many.cost == 1150, "A plan of several crafts");
        const auto partial = Sum({{2, 100}, {3, 0}, {1, -1}});
        Check(partial.cost == 200 && partial.unpriced == 2, "An unknown price is counted apart, never as free");
        Check(Sum({}).cost == 0 && Sum({}).unpriced == 0, "No ingredient");
        const auto huge = Sum({{1000000, 2000000}}, 1000);
        Check(huge.cost == 2000000000000000LL, "Large plans stay exact");
        Check(Missing(10, 3) == 7 && Missing(3, 10) == 0 && Missing(5, 5) == 0, "What is left to gather");
        std::cout << "PASS: kamas, recipe cost, unknown prices, what is missing\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
