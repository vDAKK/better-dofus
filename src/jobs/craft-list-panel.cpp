#include "jobs/craft-list-panel.hpp"
#include "jobs/craft-list.hpp"
#include "jobs/recipe-cost.hpp"
#include "jobs/recipe-native.hpp"
#include "core/module.hpp"
#include "souls/soul-panel.hpp"
#include "ui/native-click.hpp"
#include "ui/native-style.hpp"
#include "ui/tools-ui.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <optional>

namespace {
using namespace managed;
constexpr const char* Ui = "UnityEngine.UIElementsModule.dll";
constexpr const char* UiSpace = "UnityEngine.UIElements";
constexpr const char* Figma = "Core.UILogic.Components.Figma";
constexpr const char* Key = "craftlist";
constexpr int IconLines = 185, IconMagnifier = 194;
constexpr int TertiaryStyle = 2, SlotXSmall = 0, PickingIgnore = 1, AlignRight = 5;
constexpr float StockWidth = 64, PriceWidth = 84;
constexpr uint64_t RefreshMs = 1500, ObserveMs = 2000, SettleMs = 10000;
constexpr size_t MaxBuyRows = 60, MaxCraftRows = 24;

struct EntryRow { Ref line, slot, name, minus, plus, remove; int item = 0, shownIcon = 0; };
struct BuyRow { Ref line, slot, name, stock, price, select; int item = 0, shownIcon = 0; };
struct CraftRow { Ref line, slot, name, stock; int shownIcon = 0; };
struct State {
    Ref root, window, header, dragger, dependencies, cached, compositor, closeButton;
    Ref total, hint, subRecipes, entriesBox, buyHead, buyBox, craftHead, craftBox, empty, more;
    std::array<Ref, 4> inputEvents;
    std::vector<Ref> manipulators;
    std::vector<EntryRow> entries;
    std::vector<BuyRow> buys;
    std::vector<CraftRow> crafts;
    uint64_t nextRefresh = 0;
    bool cleanup = false, shownSubRecipes = false;
} state;

Method unsubscribe = nullptr, createItem = nullptr, selectFromRecipe = nullptr;
Klass selectMessage = nullptr;
using EnableFn = void (*)(Obj, Method);
EnableFn originalEnable = nullptr, originalDisable = nullptr;
Ref auctionHouse; // The auction house's buying tab while the game shows it.
bool installed = false, wanted = false, iconsFailed = false;

craftList::List list;
craftList::Tracker tracker;
std::filesystem::path dataPath;
std::filesystem::file_time_type dataStamp{};
bool dataStamped = false;
uint64_t nextObserve = 0, settledAt = 0;
struct Known { std::vector<craftList::Part> parts; bool hasRecipe = false; };
std::map<int, Known> recipes;
std::map<int, std::string> names;

bool Session() { return module && module->ready && module->servicesReady && !module->stopped && !module->stopRequested; }
Obj Style(Obj element, const char* property) {
    Ref style(api.Call(element, "get_style"));
    return api.Call(style.get(), (std::string("UnityEngine.UIElements.IStyle.get_") + property).c_str());
}
void WriteStyle(Obj element, const char* property, Obj box) {
    Ref style(api.Call(element, "get_style"));
    api.Call(style.get(), (std::string("UnityEngine.UIElements.IStyle.set_") + property).c_str(), {api.object_unbox(box)});
}
template <class T> void Set(Obj element, const char* property, T value) {
    Ref box(Style(element, property)); api.Call(box.get(), "set_value", {&value}); WriteStyle(element, property, box.get());
}
void Pixels(Obj element, const char* property, float value) {
    int unit = 0; Ref length(api.New(api.Class(Ui, UiSpace, "Length"), {&value, &unit}));
    Ref box(Style(element, property)); api.Call(box.get(), "set_value", {api.object_unbox(length.get())}); WriteStyle(element, property, box.get());
}
void Text(Obj element, const std::string& text, const char* setter = "set_text") {
    Ref value(api.string_new(text.c_str())); api.Call(element, setter, {value.get()});
}
void Add(Obj parent, Obj child) { api.Call(parent, "Add", {child}); }
void Show(Obj element, bool shown) { Set<int>(element, "display", shown ? 0 : 1); }
Obj Label(Obj parent, const std::string& text, void (*style)(Obj) = nativeStyle::Body) {
    Ref label(nativeStyle::NewLabel());
    bool rich = false; api.Call(label.get(), "set_enableRichText", {&rich});
    Set<int>(label.get(), "whiteSpace", 0);
    style(label.get());
    Text(label.get(), text);
    Add(parent, label.get());
    return label.get();
}
Obj Element(Obj parent) {
    Ref element(api.New(api.Class(Ui, UiSpace, "VisualElement")));
    Add(parent, element.get());
    return element.get();
}
Obj Row(Obj parent) {
    Ref row(Element(parent));
    Set<int>(row.get(), "flexDirection", 2); Set<int>(row.get(), "alignItems", 2);
    Pixels(row.get(), "marginBottom", 2);
    return row.get();
}
Obj Name(Obj row) {
    Ref name(Label(row, ""));
    Set<float>(name.get(), "flexGrow", 1.f); Set<float>(name.get(), "flexShrink", 1.f);
    return name.get();
}
Obj Column(Obj row, float width, void (*style)(Obj) = nativeStyle::Body) {
    Ref label(Label(row, "", style));
    Pixels(label.get(), "width", width); Set<float>(label.get(), "flexShrink", 0.f);
    Set<int>(label.get(), "unityTextAlign", AlignRight);
    return label.get();
}
Obj Button(Obj parent, const char* text, int icon, const char* tooltip) {
    Ref button(api.New(api.Class("Core.dll", Figma, "DofusButtonCustom")));
    int size = 0, style = TertiaryStyle;
    api.Call(button.get(), "set_size", {&size}); api.Call(button.get(), "set_mainStyle", {&style});
    if (icon) api.Call(button.get(), "set_icon", {&icon});
    if (*text) Text(button.get(), text);
    Text(button.get(), tooltip, "set_tooltip");
    Set<float>(button.get(), "flexShrink", 0.f);
    Pixels(button.get(), "marginLeft", 4);
    Add(parent, button.get());
    return button.get();
}
Obj NewItem(int id, int quantity) {
    int16_t position = 63; uint32_t uid = 0, count = static_cast<uint32_t>(std::max(1, quantity));
    return api.Invoke(createItem, nullptr, {&id, &position, &uid, &count, nullptr});
}
Obj Slot(Obj row) {
    Ref slot(api.New(api.Class("Core.dll", "Core.UILogic.Components.Slots", "Slot")));
    api.Call(slot.get(), "DisableDrag");
    int size = SlotXSmall, ignore = PickingIgnore;
    api.Call(slot.get(), "set_slotSize", {&size});
    api.Call(slot.get(), "set_pickingMode", {&ignore});
    Set<float>(slot.get(), "flexShrink", 0.f); Pixels(slot.get(), "marginRight", 6);
    Add(row, slot.get());
    return slot.get();
}
// The image of an item; a row without it stays readable.
void Icon(Obj slot, int& shown, int id) {
    if (shown == id || iconsFailed) return;
    try {
        Ref item(NewItem(id, 1));
        if (item.get()) api.Call(slot, "set_data", {item.get()});
        shown = id;
    } catch (const std::exception& error) {
        iconsFailed = true;
        Log(std::string("CRAFT_LIST_ICON_ERROR ") + error.what());
    }
}
const std::string& NameOf(int item) {
    const auto found = names.find(item);
    return found != names.end() ? found->second : names.emplace(item, recipeNative::ItemName(item)).first->second;
}
const std::vector<craftList::Part>* RecipeOf(int item) {
    auto found = recipes.find(item);
    if (found == recipes.end()) {
        if (!Session()) return nullptr;
        Known known;
        if (const auto recipe = recipeNative::Find(item)) {
            known.hasRecipe = true;
            for (const auto& ingredient : recipe->ingredients) {
                known.parts.push_back({ingredient.id, ingredient.quantity});
                names.emplace(ingredient.id, ingredient.name);
            }
        }
        found = recipes.emplace(item, std::move(known)).first;
    }
    return found->second.hasRecipe ? &found->second.parts : nullptr;
}

// The file can be changed by the game of another account: re-read before any change.
void Reload() {
    if (dataPath.empty()) return;
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(dataPath, error);
    if (error) { if (dataStamped) { list = {}; dataStamped = false; } return; }
    if (dataStamped && stamp == dataStamp) return;
    list = craftList::Load(dataPath);
    dataStamp = stamp; dataStamped = true;
}
void Commit() {
    if (dataPath.empty()) return;
    craftList::Save(dataPath, list);
    std::error_code error;
    dataStamp = std::filesystem::last_write_time(dataPath, error);
    dataStamped = !error;
}
template <class Change> void Mutate(Change change) {
    try { Reload(); change(); Commit(); }
    catch (const std::exception& error) { Log(std::string("CRAFT_LIST_SAVE_ERROR ") + error.what()); }
    state.nextRefresh = 0;
}

EntryRow& EntryAt(size_t index) {
    while (state.entries.size() <= index) {
        EntryRow row;
        row.line.reset(Row(state.entriesBox.get()));
        row.slot.reset(Slot(row.line.get()));
        row.name.reset(Name(row.line.get()));
        row.minus.reset(Button(row.line.get(), "−", 0, "Un de moins"));
        row.plus.reset(Button(row.line.get(), "+", 0, "Un de plus"));
        row.remove.reset(Button(row.line.get(), "×", 0, "Retirer ce craft de la liste"));
        state.entries.push_back(std::move(row));
    }
    return state.entries[index];
}
BuyRow& BuyAt(size_t index) {
    while (state.buys.size() <= index) {
        BuyRow row;
        row.line.reset(Row(state.buyBox.get()));
        row.slot.reset(Slot(row.line.get()));
        row.name.reset(Name(row.line.get()));
        row.stock.reset(Column(row.line.get(), StockWidth));
        row.price.reset(Column(row.line.get(), PriceWidth));
        row.select.reset(Button(row.line.get(), "", IconMagnifier, "Afficher cette ressource dans l’hôtel de vente ouvert"));
        state.buys.push_back(std::move(row));
    }
    return state.buys[index];
}
CraftRow& CraftAt(size_t index) {
    while (state.crafts.size() <= index) {
        CraftRow row;
        row.line.reset(Row(state.craftBox.get()));
        row.slot.reset(Slot(row.line.get()));
        row.name.reset(Name(row.line.get()));
        row.stock.reset(Column(row.line.get(), StockWidth));
        state.crafts.push_back(std::move(row));
    }
    return state.crafts[index];
}
std::string StockTooltip(const recipeNative::Stock& stock) {
    return "Sac " + std::to_string(stock.bag) + (stock.bankKnown ? ", banque " + std::to_string(stock.bank) : ", banque non lue") +
        (stock.havenBagKnown ? ", havre-sac " + std::to_string(stock.havenBag) : "");
}
void Refresh() {
    Reload();
    std::map<int, recipeNative::Stock> stocks;
    const auto stockOf = [&](int item) -> const recipeNative::Stock& {
        const auto found = stocks.find(item);
        return found != stocks.end() ? found->second : stocks.emplace(item, recipeNative::Owned(item)).first->second;
    };
    const auto plan = craftList::Compute(list, RecipeOf, [&](int item) { return stockOf(item).Total(); });

    for (size_t i = 0; i < list.entries.size(); ++i) {
        const auto& entry = list.entries[i];
        auto& row = EntryAt(i);
        row.item = entry.item;
        Show(row.line.get(), true);
        Icon(row.slot.get(), row.shownIcon, entry.item);
        const int remaining = craftList::Remaining(entry);
        std::string text = std::to_string(entry.count) + " × " + NameOf(entry.item);
        if (!remaining) text += " — terminé";
        else if (entry.made) text += " — " + std::to_string(entry.made) + " fait" + (entry.made > 1 ? "s" : "");
        else if (!RecipeOf(entry.item)) text += " — sans recette";
        Text(row.name.get(), text);
        nativeStyle::Tone(row.name.get(), remaining == 0, false);
    }
    for (size_t i = list.entries.size(); i < state.entries.size(); ++i) { state.entries[i].item = 0; Show(state.entries[i].line.get(), false); }

    std::vector<recipeCost::Line> lines;
    const bool canSelect = auctionHouse.get() != nullptr;
    const size_t buyShown = std::min(plan.buy.size(), MaxBuyRows);
    for (size_t i = 0; i < plan.buy.size(); ++i) {
        const auto& need = plan.buy[i];
        const auto price = recipeNative::AveragePrice(need.item);
        if (need.missing > 0) lines.push_back({1, price > 0 ? price * need.missing : 0});
        if (i >= buyShown) continue;
        auto& row = BuyAt(i);
        row.item = need.item;
        Show(row.line.get(), true);
        Icon(row.slot.get(), row.shownIcon, need.item);
        Text(row.name.get(), NameOf(need.item));
        Text(row.stock.get(), std::to_string(need.covered) + "/" + std::to_string(need.needed));
        Text(row.stock.get(), StockTooltip(stockOf(need.item)), "set_tooltip");
        nativeStyle::Tone(row.stock.get(), need.missing == 0, need.missing > 0);
        Text(row.price.get(), need.missing == 0 ? "" : price > 0 ? recipeCost::Kamas(price * need.missing) : "—");
        Show(row.select.get(), canSelect && need.missing > 0);
    }
    for (size_t i = buyShown; i < state.buys.size(); ++i) { state.buys[i].item = 0; Show(state.buys[i].line.get(), false); }
    Show(state.buyHead.get(), !plan.buy.empty());

    const size_t craftShown = std::min(plan.craft.size(), MaxCraftRows);
    for (size_t i = 0; i < craftShown; ++i) {
        const auto& need = plan.craft[i];
        auto& row = CraftAt(i);
        Show(row.line.get(), true);
        Icon(row.slot.get(), row.shownIcon, need.item);
        Text(row.name.get(), need.missing > 0 ? std::to_string(need.missing) + " × " + NameOf(need.item) : NameOf(need.item));
        Text(row.stock.get(), std::to_string(need.covered) + "/" + std::to_string(need.needed));
        Text(row.stock.get(), StockTooltip(stockOf(need.item)), "set_tooltip");
        nativeStyle::Tone(row.stock.get(), need.missing == 0, false);
    }
    for (size_t i = craftShown; i < state.crafts.size(); ++i) Show(state.crafts[i].line.get(), false);
    Show(state.craftHead.get(), !plan.craft.empty());

    const auto total = recipeCost::Sum(lines);
    Text(state.total.get(), "Reste à acheter : " + recipeCost::Kamas(total.cost) +
        (total.unpriced ? " (" + std::to_string(total.unpriced) + " sans prix)" : ""));
    Show(state.total.get(), !plan.buy.empty());
    std::string more;
    if (plan.buy.size() > buyShown) more = std::to_string(plan.buy.size() - buyShown) + " autres ressources, comptées dans le total.";
    if (plan.truncated) more += std::string(more.empty() ? "" : " ") + "Recettes trop imbriquées : les plus profondes sont comptées à l’achat.";
    Text(state.more.get(), more);
    Show(state.more.get(), !more.empty());
    Show(state.empty.get(), list.entries.empty());
    Text(state.hint.get(), canSelect ? "Une loupe affiche sa ressource dans l’hôtel de vente ouvert." :
        "Ouvre un hôtel de vente : une loupe y affichera chaque ressource.");
    Show(state.hint.get(), !plan.buy.empty());
    if (state.shownSubRecipes != list.subRecipes) {
        bool value = state.shownSubRecipes = list.subRecipes;
        api.Call(state.subRecipes.get(), "set_value", {&value});
    }
}
// As a click on an ingredient in the game's recipes while the auction house is open.
void SelectInAuctionHouse(int id) {
    if (!auctionHouse.get()) { state.nextRefresh = 0; return; }
    try {
        Ref item(NewItem(id, 1));
        if (!item.get()) throw std::runtime_error("no item");
        Ref message(api.New(selectMessage, {item.get()}));
        api.Invoke(selectFromRecipe, auctionHouse.get(), {message.get()});
        Log("CRAFT_LIST_SELECT item=" + std::to_string(id));
    } catch (const std::exception& error) { Log(std::string("CRAFT_LIST_SELECT_ERROR ") + error.what()); }
}
bool Action(Obj owner) {
    if (!owner || !state.window.get()) return false;
    if (owner == state.closeButton.get()) { wanted = false; return true; }
    for (const auto& row : state.entries) {
        const int item = row.item;
        if (!item) continue;
        if (owner == row.remove.get()) { Mutate([&] { craftList::Remove(list, item); }); return true; }
        const bool less = owner == row.minus.get();
        if (!less && owner != row.plus.get()) continue;
        Mutate([&] {
            for (const auto& entry : list.entries)
                if (entry.item == item) { craftList::SetCount(list, item, entry.count + (less ? -1 : 1)); break; }
        });
        return true;
    }
    for (const auto& row : state.buys)
        if (row.item && owner == row.select.get()) { SelectInAuctionHouse(row.item); return true; }
    return false;
}
Obj Root() {
    Ref hud(soul::GetUiRoot()); if (!hud.get()) return nullptr;
    Ref parent(api.Call(hud.get(), "get_parent"));
    Ref hierarchy(api.Call(parent.get(), "get_hierarchy"));
    const int count = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    if (count < 0 || count > 64) throw std::runtime_error("Craft list UI layer bounds");
    Obj result = nullptr;
    for (int i = 0; i < count; ++i) {
        Obj child = api.Call(hierarchy.get(), "get_Item", {&i});
        if (api.String(api.Call(child, "get_name")) == "RootMedium") {
            if (result) throw std::runtime_error("Ambiguous native window layer");
            result = child;
        }
    }
    return result;
}
bool Current() {
    return Session() && state.window.get() && !state.cleanup && state.cached.get() == module->cached.get() &&
        state.compositor.get() == module->compositor.get() && state.dependencies.get() == module->FindService("fpt") &&
        api.Call(state.window.get(), "get_parent") == state.root.get() && api.Call(state.window.get(), "get_panel");
}
void Retain(const char* field) {
    Ref items(api.Field(state.window.get(), field));
    const int count = api.Value<int>(api.Call(items.get(), "get_Count"));
    if (count < 0 || count > 32) throw std::runtime_error("Craft list native manipulator bounds");
    for (int i = 0; i < count; ++i) state.manipulators.emplace_back(api.Call(items.get(), "get_Item", {&i}));
}
void Create(Obj root) {
    state.root.reset(root); state.dependencies.reset(module->FindService("fpt"));
    if (!state.dependencies.get() || api.object_get_class(state.dependencies.get()) != api.Class("Core.dll", "", "goc"))
        throw std::runtime_error("Craft list native dependencies unavailable");
    state.cached.reset(api.Call(state.dependencies.get(), "bfri")); state.compositor.reset(module->compositor.get());
    Ref input(api.Call(state.dependencies.get(), "bfre"));
    if (state.cached.get() != module->cached.get() || !input.get() || api.object_get_class(input.get()) != api.Class("Core.dll", "", "eqo"))
        throw std::runtime_error("Craft list native session mismatch");
    state.window.reset(api.New(api.Class("Core.dll", "Core.UILogic.Components", "MagneticDragWindow")));
    Text(state.window.get(), "", "set_name"); Text(state.window.get(), "Liste de crafts", "set_headerTitle");
    bool yes = true; int icon = IconLines, size = 1;
    api.Call(state.window.get(), "set_headerIcon", {&icon}); api.Call(state.window.get(), "set_headerSize", {&size});
    api.Call(state.window.get(), "set_headerShowCloseButton", {&yes});
    for (const auto* setter : {"set_verticalResizable", "set_horizontalResizable", "set_cornerResizable"})
        api.Call(state.window.get(), setter, {&yes});
    api.Call(state.window.get(), "set_hasBackground", {&yes});
    Set<int>(state.window.get(), "position", 1); Set<float>(state.window.get(), "flexGrow", 0.f);
    Pixels(state.window.get(), "width", 480); Pixels(state.window.get(), "minWidth", 340); Pixels(state.window.get(), "minHeight", 260);
    Pixels(state.window.get(), "height", 520);
    Pixels(state.window.get(), "left", 160); Pixels(state.window.get(), "top", 100);
    state.header.reset(api.Call(state.window.get(), "get_headerWidget"));
    state.closeButton.reset(api.Field(state.header.get(), "closeButton"));
    if (!state.closeButton.get()) throw std::runtime_error("Craft list native close button unavailable");
    toolsUi::AttachHeader(Key, state.window.get());
    Ref draggers(api.Field(state.window.get(), "m_registeredElementDraggers"));
    state.dragger.reset(api.Call(draggers.get(), "get_Item", {state.header.get()}));
    Retain("m_elementDraggers"); Retain("m_resizerList");
    const char* getters[] = {"ydf", "ydh", "ydj", "ydl"};
    for (size_t i = 0; i < 4; ++i) state.inputEvents[i].reset(api.Call(input.get(), getters[i]));
    api.Call(state.window.get(), "Init", {state.dependencies.get()});
    Ref content(toolsUi::ScrollBody(state.window.get(), root, true));
    Set<float>(content.get(), "flexGrow", 1.f);
    for (const auto* side : {"paddingLeft", "paddingRight", "paddingTop", "paddingBottom"}) Pixels(content.get(), side, 10);

    state.empty.reset(Label(content.get(), "Aucun craft. Ouvre la recette d’un objet et clique sur Ajouter à la liste.", nativeStyle::Secondary));
    state.entriesBox.reset(Element(content.get()));
    state.subRecipes.reset(api.New(api.Class("Core.dll", Figma, "CheckboxCustom")));
    Text(state.subRecipes.get(), "Inclure les sous-recettes", "set_rightText");
    Text(state.subRecipes.get(), "Coché : un ingrédient qui se fabrique compte pour ses propres ingrédients. Sinon, il s’achète.", "set_tooltip");
    Pixels(state.subRecipes.get(), "marginTop", 6); Pixels(state.subRecipes.get(), "marginBottom", 8);
    Add(content.get(), state.subRecipes.get());
    state.craftHead.reset(Label(content.get(), "À fabriquer d’abord", nativeStyle::Heading));
    Pixels(state.craftHead.get(), "marginBottom", 4);
    state.craftBox.reset(Element(content.get()));
    Pixels(state.craftBox.get(), "marginBottom", 8);
    state.buyHead.reset(Row(content.get()));
    Pixels(state.buyHead.get(), "marginBottom", 4);
    Ref buyTitle(Label(state.buyHead.get(), "À acheter", nativeStyle::Heading));
    Set<float>(buyTitle.get(), "flexGrow", 1.f);
    Ref stockTitle(Column(state.buyHead.get(), StockWidth, nativeStyle::Heading)); Text(stockTitle.get(), "Stock");
    Ref priceTitle(Column(state.buyHead.get(), PriceWidth, nativeStyle::Heading)); Text(priceTitle.get(), "Prix");
    Pixels(priceTitle.get(), "marginRight", 32); // Above the prices, past the magnifiers' column.
    state.buyBox.reset(Element(content.get()));
    state.more.reset(Label(content.get(), "", nativeStyle::Secondary));
    state.total.reset(Label(content.get(), "", nativeStyle::Strong));
    Pixels(state.total.get(), "marginTop", 6);
    state.hint.reset(Label(content.get(), "", nativeStyle::Secondary));
    Pixels(state.hint.get(), "marginTop", 4);
    state.nextRefresh = 0;
    Refresh();
    Add(root, state.window.get());
    Log("CRAFT_LIST_OPEN crafts=" + std::to_string(list.entries.size()));
}
void EnableHook(Obj self, Method method) {
    originalEnable(self, method);
    try { auctionHouse.reset(self); state.nextRefresh = 0; } catch (...) {}
}
void DisableHook(Obj self, Method method) {
    try { if (auctionHouse.get() == self) { auctionHouse.reset(nullptr); state.nextRefresh = 0; } } catch (...) {}
    originalDisable(self, method);
}
void Observe() {
    const auto now = GetTickCount64();
    if (now < nextObserve) return;
    nextObserve = now + ObserveMs;
    Reload();
    if (list.entries.empty()) return;
    Ref hud(soul::GetUiRoot());
    if (!hud.get()) { settledAt = 0; return; }
    if (!settledAt) settledAt = now + SettleMs; // The bag fills as the character arrives.
    if (now < settledAt) return;
    const bool changed = craftList::Observe(list, tracker, [](int item) {
        const auto stock = recipeNative::Owned(item);
        craftList::Reading reading;
        reading.bag = stock.bag; reading.bank = stock.bank; reading.havenBag = stock.havenBag;
        reading.bagKnown = stock.bagKnown; reading.bankKnown = stock.bankKnown; reading.havenBagKnown = stock.havenBagKnown;
        return reading;
    });
    if (changed) { Commit(); state.nextRefresh = 0; }
}
}

bool CraftListEnabled() { return module && module->settings.craftListButton; }
int AddToCraftList(int item) {
    int count = 0;
    bool first = false;
    Mutate([&] {
        first = list.entries.empty();
        count = craftList::Add(list, item);
    });
    if (first && count) wanted = true;
    return count;
}
int CraftListCount(int item) {
    try { Reload(); } catch (const std::exception&) {}
    for (const auto& entry : list.entries) if (entry.item == item) return entry.count;
    return 0;
}

void InstallCraftList() {
    try {
        unsubscribe = api.FindVerified(api.Class("Core.dll", "Core.Engine.Inputs.Events", "InputCustomEvent"), "UnSubscribe", 1, 0x2097300);
        createItem = api.FindVerified(api.Class("Core.dll", "", "mc"), "ohc", 5, 0x2A6BF80);
        selectMessage = api.Class("Core.dll", "", "bjy");
        api.FindVerified(selectMessage, ".ctor", 1, 0x262E190);
        const auto buy = api.Class("Core.dll", "Core.UILogic.TradeCenter.Buy", "AuctionHouseBuy");
        selectFromRecipe = api.FindVerified(buy, "OnBidHouseSelectItemFromRecipe", 1, 0x2874740);
        const auto checkbox = api.Class("Core.dll", Figma, "CheckboxCustom");
        api.FindVerified(checkbox, "get_value", 0, 0x16FEAE0);
        api.FindVerified(checkbox, "set_value", 1, 0x16FF100);
        AddHook(api.FindVerified(buy, "OnEnable", 0, 0x2875A20), 0x2875A20, reinterpret_cast<void*>(&EnableHook), reinterpret_cast<void**>(&originalEnable));
        AddHook(api.FindVerified(buy, "OnDisable", 0, 0x28756A0), 0x28756A0, reinterpret_cast<void*>(&DisableHook), reinterpret_cast<void**>(&originalDisable));
        if (!module->settingsPath.empty()) dataPath = craftList::DataPath(module->settingsPath);
        Reload();
        soul::SetAuxiliaryPanelAction(Action); nativeClick::AddHandler(Action);
        toolsUi::Register(Key, "Liste de crafts", IconLines, [] { return wanted; }, [] { wanted = !wanted; return true; }, {},
            [] { return CraftListEnabled(); });
        installed = true;
        Log("CRAFT_LIST_READY crafts=" + std::to_string(list.entries.size()));
    } catch (const std::exception& error) {
        Log(std::string("CRAFT_LIST_UNAVAILABLE ") + error.what());
    }
}

void RestoreCraftList() {
    wanted = false;
    if (!state.window.get()) { state = State{}; return; }
    state.cleanup = true;
    try {
        Set<int>(state.window.get(), "display", 1);
        toolsUi::DetachHeader(Key);
        const auto mouse = api.Class(Ui, UiSpace, "MouseCaptureController");
        if (state.header.get() && api.Value<bool>(api.Invoke(api.Find(mouse, "HasMouseCapture", 1), nullptr, {state.header.get()}))) {
            *static_cast<bool*>(api.FieldAddress(state.dragger.get(), "m_active")) = false;
            api.Invoke(api.Find(mouse, "ReleaseMouse", 1), nullptr, {state.header.get()});
            reinterpret_cast<void (*)(bool, Method)>(api.NativeAt(0x16679C0))(false, nullptr);
        }
        if (state.dragger.get()) api.Call(state.dragger.get(), "RemoveSnapPreview");
        bool locked = true; int none = 0;
        api.Call(state.window.get(), "set_isUILocked", {&locked});
        api.Call(state.window.get(), "set_dragPosition", {&none});
        for (const auto& event : state.inputEvents) {
            if (!event.get()) continue;
            Ref listeners(api.Field(event.get(), "m_input")); if (!listeners.get()) continue;
            const int count = api.Value<int>(api.Call(listeners.get(), "get_Count"));
            if (count < 0 || count > 16384) throw std::runtime_error("Craft list input subscriber bounds");
            for (int i = count; i-- > 0;) {
                Ref callback(api.Call(listeners.get(), "get_Item", {&i}));
                if (!callback.get()) continue;
                Obj target = api.Call(callback.get(), "get_Target");
                if (std::any_of(state.manipulators.begin(), state.manipulators.end(), [&](const Ref& item) { return item.get() == target; }))
                    api.Invoke(unsubscribe, event.get(), {callback.get()});
            }
        }
        api.Call(state.window.get(), "RemoveFromHierarchy");
        state = State{};
    } catch (const std::exception& error) { Log(std::string("CRAFT_LIST_CLEANUP_ERROR ") + error.what()); }
}
bool CraftListCleanupPending() { return state.cleanup; }
void CloseCraftListOffWorld() {
    if (!installed) return;
    // Another character may come next, with another bag.
    tracker = {}; settledAt = 0;
    auctionHouse.reset(nullptr);
    if (wanted || state.window.get()) RestoreCraftList();
}

void TickCraftList() {
    if (!installed) return;
    if (Session()) {
        try { Observe(); }
        catch (const std::exception& error) { Log(std::string("CRAFT_LIST_OBSERVE_ERROR ") + error.what()); }
    }
    if (!wanted || !Session() || state.cleanup || (state.window.get() && !Current())) {
        if (wanted || state.window.get() || state.cleanup) {
            const bool keep = wanted && Session() && !state.cleanup;
            RestoreCraftList();
            wanted = keep;
        }
        return;
    }
    if (!state.window.get()) {
        Ref root(Root()); if (!root.get() || !api.Call(root.get(), "get_panel")) return;
        try { Create(root.get()); }
        catch (const std::exception& error) { Log(std::string("CRAFT_LIST_CREATE_ERROR ") + error.what()); RestoreCraftList(); }
        return;
    }
    try {
        const bool sub = api.Value<bool>(api.Call(state.subRecipes.get(), "get_value"));
        if (sub != state.shownSubRecipes) { state.shownSubRecipes = sub; Mutate([&] { list.subRecipes = sub; }); }
        const auto now = GetTickCount64();
        if (now >= state.nextRefresh) {
            state.nextRefresh = now + RefreshMs;
            Refresh();
        }
    } catch (const std::exception& error) { Log(std::string("CRAFT_LIST_ERROR ") + error.what()); RestoreCraftList(); }
}
