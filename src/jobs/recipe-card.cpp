#include "jobs/recipe-card.hpp"
#include "jobs/recipe-cost.hpp"
#include "jobs/recipe-native.hpp"
#include "core/module.hpp"
#include "souls/soul-panel.hpp"
#include "ui/native-style.hpp"
#include <algorithm>
#include <array>
#include <vector>

namespace {
using namespace managed;
constexpr const char* Ui = "UnityEngine.UIElementsModule.dll";
constexpr const char* UiSpace = "UnityEngine.UIElements";
constexpr const char* Builders = "Core.UILogic.Components.Tooltips.Builder";
constexpr const char* PriceLineName = "better_dofus_craft_price";
constexpr int LocationCenter = 4, SlotXSmall = 0, PickingIgnore = 1;
using MessageFn = void (*)(Obj, Obj, Method);
using BuilderFn = void (*)(Obj, Method);
// ggm's four copies of the recipe message's handler: the build's obfuscation keeps
// them apart, and which one the game subscribes is not visible in the files.
constexpr std::array<uintptr_t, 4> MessageRvas{0x1F57680, 0x1F57A90, 0x1F57DA0, 0x1F581C0};
std::array<MessageFn, 4> originalMessage{};
BuilderFn originalSetup = nullptr, originalPrices = nullptr;
Method tooltipFactory = nullptr, createItem = nullptr;
Klass tooltipServiceClass = nullptr;
// The tooltips opened as a recipe card, by their information; the game's own pinned
// tooltips stay as they are.
std::vector<Ref> cardInfos;
bool reported = false;

void Report(const char* where, const std::exception& error) {
    if (!reported) Log(std::string("RECIPE_CARD_ERROR ") + where + ": " + error.what());
    reported = true;
}
bool Enabled() { return module && module->servicesReady && !module->stopped && module->settings.recipeCard; }
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
Obj Label(Obj parent, const std::string& text, void (*style)(Obj) = nativeStyle::Body) {
    Ref label(nativeStyle::NewLabel());
    Set<int>(label.get(), "whiteSpace", 0);
    style(label.get());
    Text(label.get(), text); api.Call(parent, "Add", {label.get()});
    return label.get();
}
Obj Element(Obj parent) {
    Ref element(api.New(api.Class(Ui, UiSpace, "VisualElement")));
    api.Call(parent, "Add", {element.get()});
    return element.get();
}
int ItemId(Obj wrapper) {
    Ref data(api.Call(wrapper, "myx"));
    return data.get() ? api.Value<int>(api.Field(data.get(), "id")) : 0;
}
// The item's image, as in the recipes' own lines; a line without it stays readable.
void Icon(Obj parent, int id, int quantity) {
    try {
        int16_t position = 63; uint32_t uid = 0, count = static_cast<uint32_t>(quantity);
        Ref item(api.Invoke(createItem, nullptr, {&id, &position, &uid, &count, nullptr}));
        if (!item.get() || ItemId(item.get()) != id) return;
        Ref slot(api.New(api.Class("Core.dll", "Core.UILogic.Components.Slots", "Slot")));
        api.Call(slot.get(), "DisableDrag");
        int size = SlotXSmall, ignore = PickingIgnore;
        api.Call(slot.get(), "set_slotSize", {&size});
        api.Call(slot.get(), "set_pickingMode", {&ignore});
        api.Call(slot.get(), "set_data", {item.get()});
        Set<float>(slot.get(), "flexShrink", 0.f); Pixels(slot.get(), "marginRight", 6);
        api.Call(parent, "Add", {slot.get()});
    } catch (const std::exception& error) { Report("icon", error); }
}
constexpr float StockWidth = 54, PriceWidth = 78;
constexpr int AlignRight = 5; // TextAnchor.MiddleRight
Obj Row(Obj parent) {
    Ref row(Element(parent));
    Set<int>(row.get(), "flexDirection", 2); Set<int>(row.get(), "alignItems", 2);
    return row.get();
}
Obj Column(Obj row, const std::string& text, float width, void (*style)(Obj) = nativeStyle::Body) {
    Ref label(Label(row, text, style));
    Pixels(label.get(), "width", width); Set<float>(label.get(), "flexShrink", 0.f);
    Set<int>(label.get(), "unityTextAlign", AlignRight);
    return label.get();
}
bool IsCard(Obj builder) {
    Ref info(api.Field(builder, "m_tooltipInfos"));
    return std::any_of(cardInfos.begin(), cardInfos.end(), [&](const Ref& card) { return card.get() == info.get(); });
}
void Hide(Obj element) { if (element) Set<int>(element, "display", 1); }
// The block of a recipe card: a table of the ingredients, then the total. Everything
// that is not the recipe goes: the card answers « what does it take ». The parent is
// emptied by the tooltip's own ClearElements each time it is set up.
void AddRecipeBlock(Obj builder, const recipeNative::Recipe& recipe) {
    Ref binding(api.Field(builder, "m_binding"));
    Ref blocks(api.Call(binding.get(), "get_ctr_tooltipBlocks"));
    Ref hierarchy(api.Call(blocks.get(), "get_hierarchy"));
    const int count = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    for (int i = 0; i < count && i < 64; ++i) { Ref child(api.Call(hierarchy.get(), "get_Item", {&i})); Hide(child.get()); }
    Ref weight(api.Call(binding.get(), "get_dataSection_weight")), description(api.Call(binding.get(), "get_ctr_tooltipDescription"));
    Hide(weight.get()); Hide(description.get());
    Ref block(Element(blocks.get()));
    for (const char* side : {"paddingLeft", "paddingRight"}) Pixels(block.get(), side, 10);
    for (const char* side : {"paddingTop", "paddingBottom"}) Pixels(block.get(), side, 6);
    Ref head(Row(block.get()));
    Pixels(head.get(), "marginBottom", 4);
    Ref title(Label(head.get(), "Recette", nativeStyle::Heading));
    Set<float>(title.get(), "flexGrow", 1.f);
    Column(head.get(), "Stock", StockWidth, nativeStyle::Heading);
    Column(head.get(), "Prix", PriceWidth, nativeStyle::Heading);
    std::vector<recipeCost::Line> lines;
    for (const auto& ingredient : recipe.ingredients) {
        const auto price = recipeNative::AveragePrice(ingredient.id);
        lines.push_back({ingredient.quantity, price});
        Ref row(Row(block.get()));
        Pixels(row.get(), "marginBottom", 2);
        Icon(row.get(), ingredient.id, ingredient.quantity);
        Ref name(Label(row.get(), ingredient.name));
        Set<float>(name.get(), "flexGrow", 1.f); Set<float>(name.get(), "flexShrink", 1.f);
        const auto stock = recipeNative::Owned(ingredient.id);
        const auto owned = stock.bag + stock.bank;
        Ref have(Column(row.get(), stock.bagKnown ? std::to_string(owned) + "/" + std::to_string(ingredient.quantity) : "—", StockWidth));
        if (stock.bagKnown) nativeStyle::Tone(have.get(), owned >= ingredient.quantity, owned < ingredient.quantity);
        if (stock.bagKnown) Text(have.get(), "Sac " + std::to_string(stock.bag) + (stock.bankKnown ? ", banque " + std::to_string(stock.bank) : ", banque non lue"), "set_tooltip");
        Column(row.get(), price > 0 ? recipeCost::Kamas(price * ingredient.quantity) : "—", PriceWidth);
    }
    const auto total = recipeCost::Sum(lines);
    Ref sum(Row(block.get()));
    Pixels(sum.get(), "marginTop", 4);
    Ref sumName(Label(sum.get(), total.unpriced ? "Total (" + std::to_string(total.unpriced) + " sans prix)" : "Total", nativeStyle::Strong));
    Set<float>(sumName.get(), "flexGrow", 1.f);
    Column(sum.get(), recipeCost::Kamas(total.cost), PriceWidth + StockWidth, nativeStyle::Strong);
}
template <size_t Index> void MessageHook(Obj self, Obj message, Method method);
bool OpenCard(Obj message) {
    if (!Enabled()) return false;
    try {
        Ref item(api.Call(message, "rwq"));
        if (!item.get()) return false;
        const int id = ItemId(item.get());
        if (!id || !recipeNative::Find(id)) return false;
        Ref tooltips(module->FindService("fou")), dependencies(module->FindService("fqm")), criteria(module->FindService("fkq"));
        Ref root(soul::GetUiRoot());
        if (!tooltips.get() || !dependencies.get() || !criteria.get() || !root.get() ||
            !api.class_is_assignable_from(tooltipServiceClass, api.object_get_class(tooltips.get()))) return false;
        // As ItemTooltipBuilder.CreatePinnedTooltip: the same information, pinned.
        const auto infoClass = api.Class("Core.dll", "Core.UILogic.Components.Tooltips.Builder.Infos", "ItemTooltipInformation");
        Ref info(api.New(infoClass, {item.get()}));
        if (cardInfos.size() >= 16) cardInfos.erase(cardInfos.begin());
        cardInfos.emplace_back(info.get());
        bool yes = true, no = false;
        api.Call(info.get(), "set_pinned", {&yes}); api.Call(info.get(), "set_pinnable", {&no});
        *static_cast<bool*>(api.FieldAddress(info.get(), "effects")) = true;
        Ref position(api.New(api.Class("Core.dll", "Core.UILogic.Components.Tooltips.Builder.Internal", "TooltipPositioning"), {root.get()}));
        int center = LocationCenter;
        api.Call(position.get(), "set_sourcePlacementOrigin", {&center});
        api.Call(position.get(), "set_tooltipPlacementOrigin", {&center});
        Ref builder(api.Invoke(tooltipFactory, nullptr, {dependencies.get(), criteria.get(), info.get(), position.get()}));
        if (!builder.get()) return false;
        Ref registered(api.Call(tooltips.get(), "bfpo", {builder.get()}));
        if (!registered.get()) return false;
        Log("RECIPE_CARD_OPEN item=" + std::to_string(id));
        return true;
    } catch (const std::exception& error) { Report("open", error); return false; }
}
template <size_t Index> void MessageHook(Obj self, Obj message, Method method) {
    if (!OpenCard(message)) originalMessage[Index](self, message, method);
}
void SetupHook(Obj builder, Method method) {
    originalSetup(builder, method);
    if (!Enabled()) return;
    try {
        if (!IsCard(builder)) return;
        Ref wrapper(api.Call(builder, "get_itemWrapper"));
        const int id = wrapper.get() ? ItemId(wrapper.get()) : 0;
        if (const auto recipe = id ? recipeNative::Find(id) : std::nullopt) AddRecipeBlock(builder, *recipe);
    } catch (const std::exception& error) { Report("block", error); }
}
// The line stays with the tooltip's reused binding: found again by its name.
Obj PriceLine(Obj parent) {
    Ref hierarchy(api.Call(parent, "get_hierarchy"));
    const int count = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    for (int i = 0; i < count && i < 64; ++i) {
        Ref child(api.Call(hierarchy.get(), "get_Item", {&i}));
        if (api.String(api.Call(child.get(), "get_name")) == PriceLineName) return child.get();
    }
    Ref line(Label(parent, ""));
    Text(line.get(), PriceLineName, "set_name");
    return line.get();
}
void PricesHook(Obj builder, Method method) {
    originalPrices(builder, method);
    if (!Enabled()) return;
    try {
        Ref wrapper(api.Call(builder, "get_itemWrapper"));
        const int id = wrapper.get() ? ItemId(wrapper.get()) : 0;
        const auto recipe = id ? recipeNative::Find(id) : std::nullopt;
        Ref binding(api.Field(builder, "m_binding"));
        Ref section(api.Call(binding.get(), "get_dataSection_price"));
        Ref parent(section.get() ? api.Call(section.get(), "get_parent") : nullptr);
        if (!parent.get()) return;
        std::vector<recipeCost::Line> lines;
        if (recipe) for (const auto& ingredient : recipe->ingredients) lines.push_back({ingredient.quantity, recipeNative::AveragePrice(ingredient.id)});
        const auto total = recipeCost::Sum(lines);
        Ref line(PriceLine(parent.get()));
        const bool show = total.cost > 0 && !IsCard(builder); // A card has its total already.
        Set<int>(line.get(), "display", show ? 0 : 1);
        if (show) Text(line.get(), "Prix moyen du craft : " + recipeCost::Kamas(total.cost) + (total.unpriced ? " (incomplet)" : ""));
    } catch (const std::exception& error) { Report("price", error); }
}
template <size_t... I> void HookMessages(std::index_sequence<I...>) {
    const auto handler = api.Class("Core.dll", "", "ggm");
    constexpr std::array<const char*, 4> names{"bkmd", "dpd", "jzh", "nuy"};
    (AddHook(api.FindVerified(handler, names[I], 1, MessageRvas[I]), MessageRvas[I], reinterpret_cast<void*>(&MessageHook<I>), reinterpret_cast<void**>(&originalMessage[I])), ...);
}
}

void InstallRecipeCard() {
    try {
        recipeNative::Install();
        const auto builder = api.Class("Core.dll", Builders, "ItemTooltipBuilder");
        tooltipFactory = api.FindVerified(builder, "CreateItemTooltip", 4, 0x1609DE0);
        api.FindVerified(builder, "get_itemWrapper", 0, 0x1611A30);
        const auto info = api.Class("Core.dll", "Core.UILogic.Components.Tooltips.Builder.Infos", "ItemTooltipInformation");
        api.FindVerified(info, ".ctor", 1, 0x164CB30);
        api.FindVerified(info, "set_pinned", 1, 0xBED620);
        api.FindVerified(info, "set_pinnable", 1, 0x844870);
        const auto effects = api.class_get_field_from_name(info, "effects");
        if (!effects || api.field_get_offset(effects) != 24) throw std::runtime_error("Tooltip information layout");
        const auto binding = api.Class("Core.dll", "Core.UILogic.Bindings", "ItemTooltipBinding");
        api.FindVerified(binding, "get_ctr_tooltipBlocks", 0, 0x2F121C0);
        api.FindVerified(binding, "get_dataSection_weight", 0, 0x2F13000);
        api.FindVerified(binding, "get_ctr_tooltipDescription", 0, 0x2F12280);
        api.FindVerified(binding, "get_dataSection_price", 0, 0x2F12DC0);
        const auto item = api.Class("Core.dll", "", "mc");
        createItem = api.FindVerified(item, "ohc", 5, 0x2A6BF80);
        api.FindVerified(item, "myx", 0, 0x2A69C20);
        api.FindVerified(api.Class(Ui, UiSpace, "VisualElement"), "AddToClassList", 1, 0x55A4B60);
        api.FindVerified(api.Class(Ui, UiSpace, "VisualElement"), "EnableInClassList", 2, 0x55A8130);
        tooltipServiceClass = api.Class("Core.dll", "", "gbr");
        api.FindVerified(tooltipServiceClass, "bfpo", 1, 0x1EC99B0);
        const auto message = api.Class("Core.dll", "", "bfz");
        api.FindVerified(message, "rwq", 0, 0x68D2B0);
        HookMessages(std::make_index_sequence<4>{});
        AddHook(api.FindVerified(builder, "SetupViewWithContent", 0, 0x16108C0), 0x16108C0, reinterpret_cast<void*>(&SetupHook), reinterpret_cast<void**>(&originalSetup));
        AddHook(api.FindVerified(builder, "SetItemPrices", 0, 0x160D3B0), 0x160D3B0, reinterpret_cast<void*>(&PricesHook), reinterpret_cast<void**>(&originalPrices));
        Log("RECIPE_CARD_READY");
    } catch (const std::exception& error) {
        Log(std::string("RECIPE_CARD_UNAVAILABLE ") + error.what());
    }
}
