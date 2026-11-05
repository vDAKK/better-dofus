#include "ui/item-search.hpp"
#include "ui/item-search-policy.hpp"
#include "ui/native-click.hpp"
#include "ui/native-style.hpp"
#include "ui/tools-ui.hpp"
#include "core/module.hpp"
#include "jobs/recipe-native.hpp"
#include "souls/soul-panel.hpp"
#include <algorithm>
#include <array>

namespace {
using namespace managed;
constexpr const char* Ui = "UnityEngine.UIElementsModule.dll";
constexpr const char* UiSpace = "UnityEngine.UIElements";
constexpr const char* Figma = "Core.UILogic.Components.Figma";
constexpr const char* Key = "itemsearch";
constexpr int IconMagnifier = 194;
constexpr size_t MaxRows = 12;
constexpr uint64_t RefreshMs = 2000;
struct RowView { Ref line, name, stock; };
struct State {
    Ref root, window, header, dragger, dependencies, cached, compositor, closeButton, input, status;
    std::array<Ref, 4> inputEvents;
    std::vector<Ref> manipulators;
    std::array<RowView, MaxRows> rows;
    std::string query;
    uint64_t nextRefresh = 0;
    bool cleanup = false;
} state;
Method unsubscribe = nullptr;
bool installed = false, wanted = false;

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
Obj Label(Obj parent, void (*style)(Obj)) {
    Ref label(nativeStyle::NewLabel());
    bool rich = false; api.Call(label.get(), "set_enableRichText", {&rich});
    Set<int>(label.get(), "whiteSpace", 0);
    style(label.get());
    Add(parent, label.get());
    return label.get();
}
Obj Root() {
    // Normal windows share RootMedium, as the notebook's.
    Ref hud(soul::GetUiRoot()); if (!hud.get()) return nullptr;
    Ref parent(api.Call(hud.get(), "get_parent"));
    Ref hierarchy(api.Call(parent.get(), "get_hierarchy"));
    const int count = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    if (count < 0 || count > 64) throw std::runtime_error("Item search UI layer bounds");
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
    Ref list(api.Field(state.window.get(), field));
    const int count = api.Value<int>(api.Call(list.get(), "get_Count"));
    if (count < 0 || count > 32) throw std::runtime_error("Item search native manipulator bounds");
    for (int i = 0; i < count; ++i) state.manipulators.emplace_back(api.Call(list.get(), "get_Item", {&i}));
}
std::string StockText(const recipeNative::Stock& stock) {
    std::string text = "Sac " + std::to_string(stock.bag);
    if (stock.bankKnown) text += " · Banque " + std::to_string(stock.bank);
    if (stock.havenBagKnown) text += " · Havre-sac " + std::to_string(stock.havenBag);
    return text;
}
void Show() {
    if (!itemSearch::Searchable(state.query)) {
        for (auto& row : state.rows) Set<int>(row.line.get(), "display", 1);
        Text(state.status.get(), "Tape au moins deux lettres du nom d’un objet.");
        return;
    }
    const auto found = recipeNative::Search(state.query);
    for (size_t i = 0; i < state.rows.size(); ++i) {
        const bool shown = i < found.size();
        Set<int>(state.rows[i].line.get(), "display", shown ? 0 : 1);
        if (!shown) continue;
        Text(state.rows[i].name.get(), found[i].name);
        Text(state.rows[i].stock.get(), StockText(found[i].stock));
    }
    Text(state.status.get(), found.empty() ? "Aucun objet trouvé dans le sac ni dans la banque." :
        found.size() > state.rows.size() ? std::to_string(found.size()) + " objets : précise ta recherche." : "");
}
bool Action(Obj owner) {
    if (!owner || owner != state.closeButton.get()) return false;
    wanted = false;
    return true;
}
void Create(Obj root) {
    state.root.reset(root); state.dependencies.reset(module->FindService("fpt"));
    if (!state.dependencies.get() || api.object_get_class(state.dependencies.get()) != api.Class("Core.dll", "", "goc"))
        throw std::runtime_error("Item search native dependencies unavailable");
    state.cached.reset(api.Call(state.dependencies.get(), "bfri")); state.compositor.reset(module->compositor.get());
    Ref input(api.Call(state.dependencies.get(), "bfre"));
    if (state.cached.get() != module->cached.get() || !input.get() || api.object_get_class(input.get()) != api.Class("Core.dll", "", "eqo"))
        throw std::runtime_error("Item search native session mismatch");
    state.window.reset(api.New(api.Class("Core.dll", "Core.UILogic.Components", "MagneticDragWindow")));
    Text(state.window.get(), "", "set_name"); Text(state.window.get(), "Recherche d’objets", "set_headerTitle");
    bool yes = true; int icon = IconMagnifier, size = 1;
    api.Call(state.window.get(), "set_headerIcon", {&icon}); api.Call(state.window.get(), "set_headerSize", {&size});
    api.Call(state.window.get(), "set_headerShowCloseButton", {&yes});
    for (const auto* setter : {"set_verticalResizable", "set_horizontalResizable", "set_cornerResizable"})
        api.Call(state.window.get(), setter, {&yes});
    api.Call(state.window.get(), "set_hasBackground", {&yes});
    Set<int>(state.window.get(), "position", 1); Set<float>(state.window.get(), "flexGrow", 0.f);
    Pixels(state.window.get(), "width", 420); Pixels(state.window.get(), "minWidth", 300); Pixels(state.window.get(), "minHeight", 240);
    Pixels(state.window.get(), "left", 120); Pixels(state.window.get(), "top", 120);
    state.header.reset(api.Call(state.window.get(), "get_headerWidget"));
    state.closeButton.reset(api.Field(state.header.get(), "closeButton"));
    if (!state.closeButton.get()) throw std::runtime_error("Item search native close button unavailable");
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
    state.input.reset(api.New(api.Class("Core.dll", Figma, "TextInput")));
    Text(state.input.get(), "Nom de l’objet", "set_placeholderText");
    int maxLength = 60; api.Call(state.input.get(), "set_maxLength", {&maxLength});
    Pixels(state.input.get(), "minHeight", 32); Pixels(state.input.get(), "marginBottom", 8);
    Add(content.get(), state.input.get());
    state.status.reset(Label(content.get(), nativeStyle::Secondary));
    Pixels(state.status.get(), "marginBottom", 6);
    for (auto& row : state.rows) {
        Ref line(api.New(api.Class(Ui, UiSpace, "VisualElement")));
        Set<int>(line.get(), "display", 1); Pixels(line.get(), "marginBottom", 6);
        Add(content.get(), line.get()); row.line.reset(line.get());
        row.name.reset(Label(line.get(), nativeStyle::Body));
        row.stock.reset(Label(line.get(), nativeStyle::Secondary));
    }
    state.query.clear(); state.nextRefresh = 0;
    Show();
    Add(root, state.window.get());
    Log("ITEM_SEARCH_OPEN native=true");
}
}

void InstallItemSearch() {
    unsubscribe = api.FindVerified(api.Class("Core.dll", "Core.Engine.Inputs.Events", "InputCustomEvent"), "UnSubscribe", 1, 0x2097300);
    soul::SetAuxiliaryPanelAction(Action); nativeClick::AddHandler(Action);
    toolsUi::Register(Key, "Recherche d’objets", IconMagnifier, [] { return wanted; }, [] { wanted = !wanted; return true; }, {},
        [] { return module && module->settings.itemSearchButton; });
    installed = true;
}

void RestoreItemSearch() {
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
            Ref list(api.Field(event.get(), "m_input")); if (!list.get()) continue;
            const int count = api.Value<int>(api.Call(list.get(), "get_Count"));
            if (count < 0 || count > 16384) throw std::runtime_error("Item search input subscriber bounds");
            for (int i = count; i-- > 0;) {
                Ref callback(api.Call(list.get(), "get_Item", {&i}));
                if (!callback.get()) continue;
                Obj target = api.Call(callback.get(), "get_Target");
                if (std::any_of(state.manipulators.begin(), state.manipulators.end(), [&](const Ref& item) { return item.get() == target; }))
                    api.Invoke(unsubscribe, event.get(), {callback.get()});
            }
        }
        api.Call(state.window.get(), "RemoveFromHierarchy");
        state = State{};
    } catch (const std::exception& error) { Log(std::string("ITEM_SEARCH_CLEANUP_ERROR ") + error.what()); }
}
bool ItemSearchCleanupPending() { return state.cleanup; }
void CloseItemSearchOffWorld() { if (installed && (wanted || state.window.get())) RestoreItemSearch(); }

void TickItemSearch() {
    if (!installed) return;
    if (!wanted || !Session() || state.cleanup || (state.window.get() && !Current())) { RestoreItemSearch(); return; }
    if (!state.window.get()) {
        Ref root(Root()); if (!root.get() || !api.Call(root.get(), "get_panel")) return;
        try { Create(root.get()); }
        catch (const std::exception& error) { Log(std::string("ITEM_SEARCH_CREATE_ERROR ") + error.what()); RestoreItemSearch(); }
        return;
    }
    try {
        const auto typed = api.String(api.Call(state.input.get(), "get_text"));
        const auto now = GetTickCount64();
        if (typed != state.query || now >= state.nextRefresh) {
            state.query = typed; state.nextRefresh = now + RefreshMs;
            Show();
        }
    } catch (const std::exception& error) { Log(std::string("ITEM_SEARCH_ERROR ") + error.what()); RestoreItemSearch(); }
}
