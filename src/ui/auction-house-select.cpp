#include "ui/auction-house-select.hpp"
#include "ui/auction-house-select-policy.hpp"
#include "core/module.hpp"

namespace {
using namespace managed;
using namespace auctionHouseSelect;
constexpr const char* Sell = "Core.UILogic.TradeCenter.Sell";
constexpr int KeyLeftShift = 304, KeyRightShift = 303;
// AuctionHouseSell: its lots in the list's order, the lots selected, and its binding
// (a struct) whose list view shows them.
constexpr size_t SellLots = 1376, SellSelected = 1480, SellBinding = 752, BindingItemList = 216 - 16;
// ItemToSale, a struct of 40 bytes in the array: its item, selection flag and index.
constexpr size_t LotSize = 40, LotItem = 16 - 16, LotSelected = 36 - 16, LotIndex = 40 - 16;
// AuctionHouseSellItemLine: its tab, its lot and its binding (a struct) with the checkbox.
constexpr size_t LineSell = 16, LineLot = 24, LineBinding = 72, BindingCheckbox = 120 - 16;
constexpr size_t ArrayLength = 0x18, ArrayData = 0x20;

using ClickFn = void (*)(Obj, Method);
using ValueFn = void (*)(Obj, bool, Method);
ClickFn originalSingle = nullptr, originalCtrl = nullptr;
ValueFn originalValue = nullptr;
Method keyHeld{}, isSelected{}, updateButtons{}, updateCheckAll{}, refreshItems{}, checkboxValue{}, setCheckbox{};
// The last lot clicked without Shift, and its tab. Compared, never read through.
struct Anchor { Obj sell = nullptr; Key key; } anchor;
int depth = 0;
bool installed = false, reported = false;

struct Depth { Depth() { ++depth; } ~Depth() { --depth; } };
bool Ready() { return module && module->ready && module->servicesReady && !module->stopped && !module->stopRequested; }
void Report(const char* where, const std::exception& error) {
    if (!reported) Log(std::string("AUCTION_SELECT_ERROR ") + where + ": " + error.what());
    reported = true;
}
void Field(Klass type, const char* name, size_t offset) {
    const auto field = api.class_get_field_from_name(type, name);
    if (!field || api.field_get_offset(field) != offset) throw std::runtime_error(std::string("Auction house field layout: ") + name);
}
bool Shift() {
    int left = KeyLeftShift, right = KeyRightShift;
    return api.Value<bool>(api.Invoke(keyHeld, nullptr, {&left})) || api.Value<bool>(api.Invoke(keyHeld, nullptr, {&right}));
}
Key LineKey(Obj line) {
    return {reinterpret_cast<uintptr_t>(Read<Obj>(line, LineLot + LotItem)), Read<int>(line, LineLot + LotIndex)};
}
char* Lots(Obj sell, size_t& count) {
    const auto lots = Read<Obj>(sell, SellLots);
    count = lots ? Read<size_t>(lots, ArrayLength) : 0;
    return lots ? static_cast<char*>(lots) + ArrayData : nullptr;
}
void Remember(Obj line) {
    const auto sell = Read<Obj>(line, LineSell);
    anchor = {sell, sell ? LineKey(line) : Key{}};
}

// The positions of the anchor and the clicked lot, when both are in the same list.
struct Range { Obj sell = nullptr; size_t anchor = 0, clicked = 0; };
bool Locate(Obj line, Range& range) {
    range.sell = Read<Obj>(line, LineSell);
    if (!range.sell || range.sell != anchor.sell) return false;
    size_t count = 0;
    const auto data = Lots(range.sell, count);
    std::vector<Key> keys;
    keys.reserve(count);
    for (size_t i = 0; i < count; ++i)
        keys.push_back({reinterpret_cast<uintptr_t>(Read<Obj>(data + i * LotSize, LotItem)), Read<int>(data + i * LotSize, LotIndex)});
    const auto from = Find(keys, anchor.key), to = Find(keys, LineKey(line));
    if (!from || !to) return false;
    range.anchor = *from; range.clicked = *to;
    return true;
}
// Selects the lots between, as the game's « select all » does: the lot's flag, then
// the lot in the selected list unless the game already counts it as selected.
void SelectBetween(const Range& range) {
    const auto selected = Read<Obj>(range.sell, SellSelected);
    if (!selected) return;
    size_t count = 0;
    const auto data = Lots(range.sell, count);
    for (const auto i : Between(range.anchor, range.clicked)) {
        if (i >= count) break;
        const auto lot = data + i * LotSize;
        if (api.Value<bool>(api.Invoke(isSelected, range.sell, {lot}))) continue;
        lot[LotSelected] = 1;
        api.Call(selected, "Add", {lot});
    }
}
// The lines on screen show their checkbox again, and the buttons count the selection.
void Refresh(Obj sell) {
    if (const auto view = Read<Obj>(sell, SellBinding + BindingItemList)) api.Invoke(refreshItems, view);
    api.Invoke(updateButtons, sell);
    api.Invoke(updateCheckAll, sell);
}

void SingleHook(Obj line, Method method) {
    if (depth || !line || !Ready()) { originalSingle(line, method); return; }
    Range range;
    bool shift = false;
    try { shift = Shift(); if (!shift || !Locate(line, range)) range.sell = nullptr; }
    catch (const std::exception& error) { Report("click", error); range.sell = nullptr; }
    if (!range.sell) {
        originalSingle(line, method);
        try { Remember(line); } catch (const std::exception& error) { Report("anchor", error); }
        return;
    }
    // Instead of the game's Shift click: the lots between, then this one through
    // its checkbox, which runs the game's own selection of a lot.
    Depth inside;
    try {
        SelectBetween(range);
        const auto checkbox = Read<Obj>(line, LineBinding + BindingCheckbox);
        if (checkbox && !api.Value<bool>(api.Invoke(checkboxValue, checkbox))) { bool value = true; api.Invoke(setCheckbox, checkbox, {&value}); }
        Refresh(range.sell);
    } catch (const std::exception& error) { Report("range", error); }
}
void CtrlHook(Obj line, Method method) {
    originalCtrl(line, method);
    if (depth || !line || !Ready()) return;
    try { Remember(line); } catch (const std::exception& error) { Report("anchor", error); }
}
void ValueHook(Obj line, bool value, Method method) {
    if (depth || !line || !Ready()) { originalValue(line, value, method); return; }
    Range range;
    try { if (!value || !Shift() || !Locate(line, range)) range.sell = nullptr; }
    catch (const std::exception& error) { Report("checkbox", error); range.sell = nullptr; }
    if (!range.sell) {
        originalValue(line, value, method);
        try { Remember(line); } catch (const std::exception& error) { Report("anchor", error); }
        return;
    }
    Depth inside;
    try { SelectBetween(range); } catch (const std::exception& error) { Report("range", error); }
    originalValue(line, value, method);
    try { Refresh(range.sell); } catch (const std::exception& error) { Report("refresh", error); }
}
}

void InstallAuctionHouseSelect() {
    if (installed) return;
    try {
        const auto sell = api.Class("Core.dll", Sell, "AuctionHouseSell");
        const auto line = api.Class("Core.dll", Sell, "AuctionHouseSellItemLine");
        Field(sell, "itemsToSale", SellLots); Field(sell, "allItemsToRemove", SellSelected); Field(sell, "binding", SellBinding);
        Field(api.Class("Core.dll", "Core.UILogic.Bindings", "AuctionHouseSellBinding"), "_gd_itemList", BindingItemList + 16);
        Field(line, "m_auctionHouseSell", LineSell); Field(line, "m_item", LineLot); Field(line, "m_itemLineBinding", LineBinding);
        Field(api.Class("Core.dll", "Core.UILogic.Bindings", "AuctionHouseSell_ItemLineBinding"), "_chk_remove", BindingCheckbox + 16);
        isSelected = api.FindVerified(sell, "ItemSelectedToBeRemoved", 1, 0x2830D00);
        // The lot's struct, from the parameter that takes it.
        const auto lot = api.class_from_type(api.method_get_param(isSelected, 0));
        if (!lot || !api.class_is_valuetype(lot)) throw std::runtime_error("Auction house lot type");
        Field(lot, "itemWrapper", LotItem + 16); Field(lot, "selected", LotSelected + 16); Field(lot, "index", LotIndex + 16);
        Field(lot, "isInit", 48); // The last field: with the item's alignment, 40 bytes.
        updateButtons = api.FindVerified(sell, "UpdateButtonsState", 0, 0x2840150);
        updateCheckAll = api.FindVerified(sell, "UpdateChkAllState", 0, 0x2840810);
        refreshItems = api.FindVerified(api.Class("UnityEngine.UIElementsModule.dll", "UnityEngine.UIElements", "BaseVerticalCollectionView"), "RefreshItems", 0, 0x5578EE0);
        const auto checkbox = api.Class("Core.dll", "Core.UILogic.Components.Figma", "CheckboxCustom");
        checkboxValue = api.FindVerified(checkbox, "get_value", 0, 0x16FEAE0);
        setCheckbox = api.FindVerified(checkbox, "set_value", 1, 0x16FF100);
        keyHeld = api.FindVerified(api.Class("UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input"), "GetKey", 1, 0x54963B0);
        AddHook(api.FindVerified(line, "OnLineSingleClicked", 0, 0x285CFD0), 0x285CFD0, reinterpret_cast<void*>(&SingleHook), reinterpret_cast<void**>(&originalSingle));
        AddHook(api.FindVerified(line, "OnLineCtrlClicked", 0, 0x285CF20), 0x285CF20, reinterpret_cast<void*>(&CtrlHook), reinterpret_cast<void**>(&originalCtrl));
        AddHook(api.FindVerified(line, "OnCheckboxValueChanged", 1, 0x285CDF0), 0x285CDF0, reinterpret_cast<void*>(&ValueHook), reinterpret_cast<void**>(&originalValue));
        installed = true;
        Log("AUCTION_SELECT_READY");
    } catch (const std::exception& error) { Log(std::string("AUCTION_SELECT_UNAVAILABLE ") + error.what()); }
}
