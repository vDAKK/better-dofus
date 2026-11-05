#include "jobs/recipe-card.hpp"
#include "jobs/recipe-cost.hpp"
#include "jobs/recipe-native.hpp"
#include "core/module.hpp"
#include "souls/soul-panel.hpp"
#include <array>

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
Obj Label(Obj parent, const std::string& text) {
    Ref label(api.New(api.Class("Core.dll", "Core.UILogic.Components", "DofusLabel")));
    bool multiline = false; api.Call(label.get(), "set_isSingleLine", {&multiline});
    Set<int>(label.get(), "whiteSpace", 0);
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
std::string StockText(const recipeNative::Stock& stock) {
    if (!stock.bagKnown) return "";
    return "sac " + std::to_string(stock.bag) + (stock.bankKnown ? ", banque " + std::to_string(stock.bank) : "");
}
// The block of a pinned tooltip. Its parent is emptied by the tooltip's own
// ClearElements each time it is set up, so nothing here needs a cleanup.
void AddRecipeBlock(Obj builder, const recipeNative::Recipe& recipe) {
    Ref binding(api.Field(builder, "m_binding"));
    Ref blocks(api.Call(binding.get(), "get_ctr_tooltipBlocks"));
    Ref block(Element(blocks.get()));
    for (const char* side : {"paddingLeft", "paddingRight", "paddingTop", "paddingBottom"}) Pixels(block.get(), side, 8);
    Ref title(Label(block.get(), "Recette · niveau " + std::to_string(recipe.level)));
    Set<int>(title.get(), "unityFontStyleAndWeight", 1); Pixels(title.get(), "marginBottom", 6);
    std::vector<recipeCost::Line> lines;
    for (const auto& ingredient : recipe.ingredients) {
        const auto price = recipeNative::AveragePrice(ingredient.id);
        lines.push_back({ingredient.quantity, price});
        Ref row(Element(block.get()));
        Set<int>(row.get(), "flexDirection", 2); Set<int>(row.get(), "alignItems", 2); Pixels(row.get(), "marginBottom", 3);
        Icon(row.get(), ingredient.id, ingredient.quantity);
        const auto stock = StockText(recipeNative::Owned(ingredient.id));
        Ref name(Label(row.get(), std::to_string(ingredient.quantity) + " × " + ingredient.name + (stock.empty() ? "" : " (" + stock + ")")));
        Set<float>(name.get(), "flexGrow", 1.f); Set<float>(name.get(), "flexShrink", 1.f);
        Ref cost(Label(row.get(), price > 0 ? recipeCost::Kamas(price * ingredient.quantity) : "—"));
        Set<float>(cost.get(), "flexShrink", 0.f); Pixels(cost.get(), "marginLeft", 8);
    }
    const auto total = recipeCost::Sum(lines);
    Ref sum(Label(block.get(), "Prix moyen du craft : " + recipeCost::Kamas(total.cost) +
        (total.unpriced ? " (sans " + std::to_string(total.unpriced) + " ingrédient(s) sans prix)" : "")));
    Set<int>(sum.get(), "unityFontStyleAndWeight", 1); Pixels(sum.get(), "marginTop", 6);
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
        if (!api.Value<bool>(api.Call(builder, "get_isPinned"))) return;
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
        Set<int>(line.get(), "display", total.cost > 0 ? 0 : 1);
        if (total.cost > 0) Text(line.get(), "Prix moyen du craft : " + recipeCost::Kamas(total.cost) + (total.unpriced ? " (incomplet)" : ""));
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
        api.FindVerified(builder, "get_isPinned", 0, 0x3965650);
        const auto info = api.Class("Core.dll", "Core.UILogic.Components.Tooltips.Builder.Infos", "ItemTooltipInformation");
        api.FindVerified(info, ".ctor", 1, 0x164CB30);
        api.FindVerified(info, "set_pinned", 1, 0xBED620);
        api.FindVerified(info, "set_pinnable", 1, 0x844870);
        const auto effects = api.class_get_field_from_name(info, "effects");
        if (!effects || api.field_get_offset(effects) != 24) throw std::runtime_error("Tooltip information layout");
        const auto binding = api.Class("Core.dll", "Core.UILogic.Bindings", "ItemTooltipBinding");
        api.FindVerified(binding, "get_ctr_tooltipBlocks", 0, 0x2F121C0);
        api.FindVerified(binding, "get_dataSection_price", 0, 0x2F12DC0);
        const auto item = api.Class("Core.dll", "", "mc");
        createItem = api.FindVerified(item, "ohc", 5, 0x2A6BF80);
        api.FindVerified(item, "myx", 0, 0x2A69C20);
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
