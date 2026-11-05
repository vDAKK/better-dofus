#include "jobs/craft-list.hpp"
#include "jobs/recipe-cost.hpp"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace craftList;

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

namespace {
// Ids: 1 Voile (2×10 + 1×20), 2 Coiffe (3×10 + 2×30), 20 and 30 craftable, 40 and 50 raw.
const std::map<int, std::vector<Part>> Recipes{
    {1, {{10, 2}, {20, 1}}},
    {2, {{10, 3}, {30, 2}}},
    {20, {{40, 4}}},
    {30, {{20, 1}, {50, 5}}},
    {90, {{91, 1}}}, // A cycle.
    {91, {{90, 1}, {50, 1}}},
};
RecipeOf Of() {
    return [](int item) -> const std::vector<Part>* {
        const auto found = Recipes.find(item);
        return found == Recipes.end() ? nullptr : &found->second;
    };
}
OwnedOf Stock(std::map<int, int64_t> owned = {}) {
    return [owned](int item) { const auto found = owned.find(item); return found == owned.end() ? int64_t{0} : found->second; };
}
const Need* Find(const std::vector<Need>& needs, int item) {
    for (const auto& need : needs) if (need.item == item) return &need;
    return nullptr;
}
}

static void Editing() {
    List list;
    Check(Add(list, 1) == 1 && Add(list, 1, 4) == 5 && list.entries.size() == 1, "Adding the same craft raises its count");
    Check(Add(list, 2, MaxCount + 5) == MaxCount, "The count is bounded");
    Check(Add(list, 2, 3) == MaxCount, "The count stays bounded");
    Check(Add(list, 0) == 0 && Add(list, 3, 0) == 0 && list.entries.size() == 2, "A bad item or count adds nothing");
    list.entries[0].made = 4;
    SetCount(list, 1, 2);
    Check(list.entries[0].count == 2 && list.entries[0].made == 2 && Remaining(list.entries[0]) == 0, "Lowering the count keeps made within it");
    SetCount(list, 1, 0);
    Check(list.entries.size() == 1 && list.entries[0].item == 2, "A count of zero removes the craft");
    Remove(list, 2);
    Check(list.entries.empty(), "Removal");
    for (int item = 1; item <= static_cast<int>(MaxEntries); ++item) Check(Add(list, item) == 1, "Room in the list");
    Check(Add(list, 1000) == 0 && list.entries.size() == MaxEntries, "A full list refuses a new craft");
    Check(Add(list, 1) == 2, "A full list still counts up");
}

static void Aggregation() {
    List list;
    Add(list, 1, 5); // 10 of 10, 5 of 20.
    Add(list, 2, 4); // 12 of 10, 8 of 30.
    auto plan = Compute(list, Of(), Stock({{10, 7}, {30, 100}}));
    const auto ten = Find(plan.buy, 10);
    Check(ten && ten->needed == 22 && ten->covered == 7 && ten->missing == 15, "A resource shared by two recipes merges, stock counted once");
    const auto thirty = Find(plan.buy, 30);
    Check(thirty && thirty->needed == 8 && thirty->covered == 8 && thirty->missing == 0, "More than enough covers the need");
    Check(plan.buy.back().missing == 0 && plan.buy.front().missing > 0, "The finished lines come last");
    Check(plan.craft.empty() && !plan.truncated, "Without sub-recipes everything is bought");
    Check(Find(plan.buy, 20) && !Find(plan.buy, 40), "Without sub-recipes a craftable ingredient is bought");
    plan = Compute(List{}, Of(), Stock());
    Check(plan.buy.empty() && plan.craft.empty(), "An empty list needs nothing");
    List unknown; Add(unknown, 777, 2);
    Check(Compute(unknown, Of(), Stock()).buy.empty(), "A craft without recipe asks for nothing");
}

static void SubRecipes() {
    List list;
    list.subRecipes = true;
    Add(list, 1, 5); // 10 of 10, 5 of 20.
    auto plan = Compute(list, Of(), Stock({{20, 2}, {40, 3}}));
    const auto twenty = Find(plan.craft, 20);
    Check(twenty && twenty->needed == 5 && twenty->covered == 2 && twenty->missing == 3, "What is owned of a sub-recipe is not crafted");
    Check(!Find(plan.buy, 20), "A sub-recipe is not bought");
    const auto forty = Find(plan.buy, 40);
    Check(forty && forty->needed == 12 && forty->covered == 3 && forty->missing == 9, "Only what is missing asks for ingredients");

    List two;
    two.subRecipes = true;
    Add(two, 1, 1); // 1 of 20.
    Add(two, 2, 1); // 2 of 30, each 1 of 20 and 5 of 50.
    plan = Compute(two, Of(), Stock({{20, 2}}));
    Check(Find(plan.craft, 20)->needed == 3 && Find(plan.craft, 20)->missing == 1, "The same sub-recipe at two depths shares its stock");
    Check(Find(plan.buy, 40)->needed == 4 && Find(plan.buy, 50)->needed == 10, "Deep ingredients add up");

    List full;
    full.subRecipes = true;
    Add(full, 1, 2);
    plan = Compute(full, Of(), Stock({{20, 2}}));
    Check(Find(plan.craft, 20)->missing == 0 && !Find(plan.buy, 40), "A covered sub-recipe asks for nothing");

    List cycle;
    cycle.subRecipes = true;
    Add(cycle, 90, 1);
    plan = Compute(cycle, Of(), Stock());
    Check(Find(plan.craft, 91) && Find(plan.buy, 90) && Find(plan.buy, 50), "A recipe needing itself ends, the loop being bought");

    // Deeper than the bound: stops and says so.
    std::map<int, std::vector<Part>> chain;
    for (int item = 1; item <= MaxDepth + 5; ++item) chain[item] = {{item + 1, 1}};
    const RecipeOf chainOf = [&](int item) -> const std::vector<Part>* { const auto found = chain.find(item); return found == chain.end() ? nullptr : &found->second; };
    List deep;
    deep.subRecipes = true;
    Add(deep, 1);
    plan = Compute(deep, chainOf, Stock());
    Check(plan.truncated && plan.buy.size() == 1, "A chain too deep is bought at the bound and reported");

    // Wider than the bound: every level doubles.
    std::map<int, std::vector<Part>> wide;
    for (int item = 1; item <= 6; ++item) { auto& parts = wide[item]; for (int part = 0; part < 30; ++part) parts.push_back({item + 1, 1}); }
    const RecipeOf wideOf = [&](int item) -> const std::vector<Part>* { const auto found = wide.find(item); return found == wide.end() ? nullptr : &found->second; };
    List broad;
    broad.subRecipes = true;
    Add(broad, 1);
    Check(Compute(broad, wideOf, Stock()).truncated, "A walk too long stops and is reported");

    // Sizes that would overflow.
    List huge;
    Add(huge, 1, MaxCount);
    const std::map<int, std::vector<Part>> big{{1, {{2, 2000000000}}}};
    const RecipeOf bigOf = [&](int item) -> const std::vector<Part>* { const auto found = big.find(item); return found == big.end() ? nullptr : &found->second; };
    Check(Find(Compute(huge, bigOf, Stock()).buy, 2)->needed > 0, "A huge quantity stays positive");
}

static void Progress() {
    List list;
    Add(list, 1, 9);
    Tracker tracker;
    Reading reading;
    reading.bagKnown = true; reading.bag = 3;
    const auto read = [&](int) { return reading; };
    Check(!Observe(list, tracker, read) && list.entries[0].made == 0, "What was owned before does not count");
    reading.bag = 5;
    Check(Observe(list, tracker, read) && list.entries[0].made == 2, "A gain counts");
    Check(Remaining(list.entries[0]) == 7, "A gain lowers what remains");
    reading.bag = 1;
    Check(!Observe(list, tracker, read) && list.entries[0].made == 2, "Selling keeps what was made");
    reading.bag = 2;
    Check(Observe(list, tracker, read) && list.entries[0].made == 3, "The next gain starts from the new quantity");
    reading.bankKnown = true; reading.bank = 40;
    Check(!Observe(list, tracker, read) && list.entries[0].made == 3, "The bank just read is not a gain");
    reading.bag = 7; reading.bank = 35;
    Check(!Observe(list, tracker, read) && list.entries[0].made == 3, "A withdrawal from the bank is not a gain");
    const auto kept = reading;
    reading = {};
    Check(!Observe(list, tracker, read), "Not knowing changes nothing");
    reading = kept; reading.bag = 8;
    Check(Observe(list, tracker, read) && list.entries[0].made == 4, "The last quantity survives a moment without data");
    reading.bag = 9; reading.havenBagKnown = true; reading.havenBag = 30;
    Check(Observe(list, tracker, read) && list.entries[0].made == 5, "A first craft counts while the havenbag becomes known");
    reading.bag = 8; reading.havenBag = 31;
    Check(!Observe(list, tracker, read) && list.entries[0].made == 5, "A move to the havenbag is not a gain");
    reading.bag = 108;
    Check(Observe(list, tracker, read) && list.entries[0].made == 9, "Made stops at the count");
    reading.bag = 208;
    Check(!Observe(list, tracker, read), "A finished craft stays as it is");

    // Crafting: the ingredients go, the result comes, the need falls with it.
    List craft;
    Add(craft, 1, 5);
    int64_t tens = 10;
    Check(Find(Compute(craft, Of(), [&](int item) { return item == 10 ? tens : int64_t{0}; }).buy, 10)->missing == 0, "Enough at first");
    craft.entries[0].made = 2; tens = 6;
    Check(Find(Compute(craft, Of(), [&](int item) { return item == 10 ? tens : int64_t{0}; }).buy, 10)->missing == 0, "Crafting does not raise the need");
    Tracker removed;
    List gone;
    Add(gone, 1);
    Observe(gone, removed, read);
    Remove(gone, 1);
    Observe(gone, removed, read);
    Check(removed.seen.empty(), "A removed craft is forgotten");
}

static void Persistence() {
    List list;
    list.subRecipes = true;
    Add(list, 1, 5);
    Add(list, 2, 4);
    list.entries[1].made = 3;
    const auto back = Parse(Serialize(list));
    Check(back.subRecipes && back.entries.size() == 2 && back.entries[1].item == 2 && back.entries[1].count == 4 && back.entries[1].made == 3, "Round trip");
    const auto bounded = Parse(R"({"entries":[{"item":1,"count":3,"made":9},{"item":1,"count":8},{"item":-1,"count":2},{"item":2,"count":0},{"item":3,"count":99999},{"item":4,"count":2,"made":-5}]})");
    Check(bounded.entries.size() == 2 && bounded.entries[0].made == 3 && bounded.entries[0].count == 3 && bounded.entries[1].item == 4 && bounded.entries[1].made == 0 && !bounded.subRecipes, "Out of bounds entries go");
    for (const char* text : {"", "nope", "[]", "{}", R"({"entries":3})", R"({"entries":[{"item":"a","count":1}]})"}) {
        bool thrown = false;
        try { Parse(text); } catch (const std::exception&) { thrown = true; }
        Check(thrown, "A corrupt text throws");
    }
    Check(DataPath(L"C:\\x\\BetterDofus-settings.ini") == std::filesystem::path(L"C:\\x\\BetterDofus-craft-list.json"), "The file sits beside the settings");

    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    const auto folder = std::filesystem::path(buffer) / (L"craft-list-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    const auto path = folder / L"BetterDofus-craft-list.json";
    Check(Load(path).entries.empty(), "No file, no craft");
    Save(path, list);
    Check(Load(path).entries.size() == 2, "Saved then loaded");
    size_t files = 0;
    for (const auto& file : std::filesystem::directory_iterator(folder)) { (void)file; ++files; }
    Check(files == 1, "No temporary file stays");
    { std::ofstream corrupt(path, std::ios::binary | std::ios::trunc); corrupt << "{broken"; }
    Check(Load(path).entries.empty(), "A corrupt file reads as empty");
    auto aside = path; aside += L".corrupt";
    Check(std::filesystem::exists(aside) && !std::filesystem::exists(path), "A corrupt file is kept aside");
    std::filesystem::remove_all(folder);
}

static void Margin() {
    using namespace recipeCost;
    Check(Estimate({1000, 0}, 1500).known && Estimate({1000, 0}, 1500).value == 500, "Gain");
    Check(Estimate({1000, 0}, 400).value == -600, "Loss");
    Check(!Estimate({1000, 1}, 1500).known, "An ingredient without price leaves no margin");
    Check(!Estimate({1000, 0}, 0).known && !Estimate({0, 0}, 500).known, "An unknown price leaves no margin");
}

int main() {
    try {
        Editing();
        Aggregation();
        SubRecipes();
        Progress();
        Persistence();
        Margin();
        std::cout << "PASS: craft list\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
