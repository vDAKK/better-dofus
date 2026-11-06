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

        Item tipped = Make(1, 1, false);
        tipped.name = "Pious"; tipped.points = 10; tipped.description = "Vaincre";
        Check(Tip(tipped, {}) == "Pious\nSuccès à faire : 10 points\nVaincre", "A tip without objectives");
        Check(Tip(tipped, {"[x] Piou Bleu", "[ ] Piou Vert"}) == "Pious\nSuccès à faire : 10 points\nVaincre\n[x] Piou Bleu\n[ ] Piou Vert", "A tip with objectives");
        tipped.description.clear();
        Check(Tip(tipped, {}) == "Pious\nSuccès à faire : 10 points", "A tip without description");
        Check(MonsterOf("(Ef>4818,0)") == 4818 && MonsterOf("Ef>7") == 7, "The monster of a criterion");
        Check(MonsterOf("PL>14") == 0 && MonsterOf("") == 0 && MonsterOf("Ef>") == 0, "A criterion without monster");
        Check(MonsterOf("Ef>99999999999999") == 0, "An absurd monster id");
        const std::vector<Objective> objectives{{1, 10, "A"}, {2, 20, "B"}, {3, 0, "Niveau 15"}};
        Check(Advances(objectives, {}, {10}), "A missing monster among the enemies");
        Check(!Advances(objectives, {1}, {10}), "That monster is done");
        Check(Advances(objectives, {1}, {10, 20}), "Another one is missing");
        Check(!Advances(objectives, {}, {30}), "No enemy of the achievement");
        Check(!Advances(objectives, {1, 2}, {10, 20}), "All monsters done, the level left");
        Check(Advances({{3, 0, "Niveau 15"}}, {}, {30}) && Advances({}, {}, {30}), "An achievement naming no monster stays");
        Check(ObjectiveLine("A", true, true) == "[x] A" && ObjectiveLine("A", true, false) == "[ ] A" && ObjectiveLine("A", false, false) == "- A", "An objective's line");
        Check(Measure(objectives, {1}).done == 1 && Measure(objectives, {1}).total == 2 && Percent(Measure(objectives, {1})) == 50, "The level does not count");
        Check(Measure(objectives, {3}).done == 0 && Percent(Progress{}) == 0, "Only the monsters count");
        Check(Validated({9, 10}) == "90 % validé" && Remaining({9, 10}) == "1 monstre restant" && Remaining({0, 2}) == "2 monstres restants", "The summary");
        Check(MarkOf(10, objectives, {}) == Mark::Missing && MarkOf(10, objectives, {1}) == Mark::Done && MarkOf(30, objectives, {}) == Mark::None, "A monster's mark");
        Check(MarkOf(10, {{1, 10, "A"}, {5, 10, "A bis"}}, {1}) == Mark::Missing, "Still missing in another objective");
        Check(ObjectiveOnMonster({5, 10}, 10, "vaincre 5 bouftou", "bouftou"), "An objective on this monster");
        Check(!ObjectiveOnMonster({5, 10}, 11, "vaincre 5 bouftou", "bouftou"), "Another monster");
        Check(!ObjectiveOnMonster({10}, 10, "ramener 5 laine", "bouftou"), "An item with the same id");
        Check(!ObjectiveOnMonster({10}, 10, "texte", ""), "A monster without a name");
        Check(Gathered(26, 50) == "26 sur 50" && Gathered(0, 50) == "0 sur 50", "What is gathered");
        Check(Gathered(80, 50) == "50 sur 50" && Gathered(-3, 50) == "0 sur 50", "Within what is asked");
        Check(Title({}).empty() && Title({"Bouftou"}) == "Bouftou", "One monster");
        Check(Title({"Bouftou", "Tofu"}) == "Bouftou et 1 autre" && Title({"Bouftou", "Tofu", "Larve"}) == "Bouftou et 2 autres", "Several monsters");
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
