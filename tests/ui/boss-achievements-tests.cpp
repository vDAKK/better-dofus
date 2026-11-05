#include "ui/boss-achievements-policy.hpp"
#include <iostream>
#include <stdexcept>

using namespace bossAchievements;

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

static Item Make(int id, int level, bool done) {
    Item item;
    item.id = id; item.level = level; item.done = done;
    return item;
}

int main() {
    try {
        Check(Merge({{1, 2}, {2, 3, 1}}) == std::vector<int>{1, 2, 3}, "Each achievement once, in order");
        Check(Merge({{0, -4, 7}}) == std::vector<int>{7}, "Bad ids go");
        Check(Merge({}).empty() && Merge({{}, {}}).empty(), "Nothing to show");
        std::vector<int> many;
        for (int id = 1; id <= 200; ++id) many.push_back(id);
        Check(Merge({many}).size() == MaxItems, "The list is bounded");

        const auto sorted = Sort({Make(1, 50, false), Make(2, 20, true), Make(3, 10, false), Make(4, 50, false), Make(5, 5, true)});
        Check(sorted.todo.size() == 3 && sorted.done.size() == 2, "Split by finished");
        Check(sorted.todo[0].id == 3 && sorted.todo[1].id == 1 && sorted.todo[2].id == 4, "Lowest level first, equal levels in the game's order");
        Check(sorted.done[0].id == 5 && sorted.done[1].id == 2, "The finished ones are sorted too");

        Check(Plain("Vaincre {monster,123::Le Comte} seul") == "Vaincre Le Comte seul", "A link reads as its name");
        Check(Plain("{a::B} et {c,1::D}") == "B et D", "Several links");
        Check(Plain("Sans lien") == "Sans lien" && Plain("").empty(), "No link");
        Check(Plain("Ouvert { sans fin") == "Ouvert { sans fin", "An open brace stays");
        Check(Plain("Bloc {sans nom} ici") == "Bloc {sans nom} ici", "Braces without a name stay");
        Check(Plain("{x} puis {a::B}") == "{x} puis B", "A plain brace before a link stays");

        Check(MenuLabel("Kardorim", Sort({Make(1, 1, false), Make(2, 1, true)}), true) == "Succès : Kardorim (1 à faire)", "To do");
        Check(MenuLabel("Kardorim", Sort({Make(2, 1, true)}), true) == "Succès : Kardorim (tous faits)", "All done");
        Check(MenuLabel("Kardorim", Sort({Make(1, 1, false)}), false) == "Succès : Kardorim", "No count without the game's list");
        std::cout << "PASS: boss achievements\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
