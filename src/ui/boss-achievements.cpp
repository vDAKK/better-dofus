#include "ui/boss-achievements.hpp"
#include "ui/boss-achievements-policy.hpp"
#include "ui/native-click.hpp"
#include "ui/native-style.hpp"
#include "ui/tools-ui.hpp"
#include "core/module.hpp"
#include "souls/soul-group-menu.hpp"
#include "souls/soul-panel.hpp"
#include <algorithm>
#include <array>
#include <optional>

namespace {
using namespace managed;
using namespace bossAchievements;
constexpr const char* DataImage = "Ankama.Dofus.Core.DataCenter.dll";
constexpr const char* QuestSpace = "Core.DataCenter.Metadata.Quest";
constexpr const char* Ui = "UnityEngine.UIElementsModule.dll";
constexpr const char* UiSpace = "UnityEngine.UIElements";
constexpr const char* Key = "bossachievements";
constexpr int IconFlag = 145, AlignRight = 5;
constexpr float PointsWidth = 60;
constexpr uint64_t RefreshMs = 2000;

struct RowView { Ref line, name, points, description; };
struct State {
    Ref root, window, header, dragger, dependencies, cached, compositor, closeButton;
    Ref status, todoHead, todoBox, doneHead, doneBox;
    std::array<Ref, 4> inputEvents;
    std::vector<Ref> manipulators;
    std::vector<RowView> todo, done;
    uint64_t nextRefresh = 0;
    bool cleanup = false;
} state;
struct Target { std::string boss; std::vector<int> ids; };
Target shown; // What the window shows.
struct MenuRow { Ref item, callback; Target target; bool armed = false; } row;

Method achievementsRoot = nullptr, monstersRoot = nullptr, dungeonsRoot = nullptr, achievementById = nullptr, monsterById = nullptr, dungeonByMap = nullptr;
Method isBoss = nullptr, monsterName = nullptr, achievementName = nullptr, achievementDescription = nullptr, finishedA = nullptr, finishedB = nullptr;
Method currentMap = nullptr, append = nullptr, unsubscribe = nullptr;
Klass serviceClass = nullptr;
bool installed = false, wanted = false, newTarget = false;

void Field(Klass type, const char* name, size_t offset) {
    const auto field = api.class_get_field_from_name(type, name);
    if (!field || api.field_get_offset(field) != offset) throw std::runtime_error(std::string("Boss achievements field layout: ") + name);
}
bool Session() { return module && module->ready && module->servicesReady && !module->stopped && !module->stopRequested; }
bool Enabled() { return Session() && module->settings.bossAchievements; }
// The element type of these lists is not in the dump: read it from the value.
int Number(Obj box) {
    const std::string type = api.class_get_name(api.object_get_class(box));
    if (type == "Int32" || type == "UInt32") return api.Value<int>(box);
    if (type == "Int16") return api.Value<short>(box);
    if (type == "UInt16") return api.Value<unsigned short>(box);
    throw std::runtime_error("Boss achievements list of " + type);
}
std::vector<int> Numbers(Obj list) {
    std::vector<int> result;
    const int count = list ? api.Value<int>(api.Call(list, "get_Count")) : 0;
    if (count < 0 || count > 512) throw std::runtime_error("Boss achievements list bounds");
    for (int i = 0; i < count; ++i) { Ref value(api.Call(list, "get_Item", {&i})); result.push_back(Number(value.get())); }
    return result;
}
// What the game knows to be finished: the two sets of its service.
struct Finished { Ref a, b; bool known = false; };
Finished ReadFinished() {
    Finished finished;
    Obj service = module->FindService("fja");
    if (!service || api.object_get_class(service) != serviceClass) return finished;
    finished.a.reset(api.Invoke(finishedA, service));
    finished.b.reset(api.Invoke(finishedB, service));
    const auto count = [](Obj set) { return set ? api.Value<int>(api.Call(set, "get_Count")) : 0; };
    finished.known = count(finished.a.get()) + count(finished.b.get()) > 0;
    return finished;
}
bool Contains(Obj set, int id) { return set && api.Value<bool>(api.Call(set, "Contains", {&id})); }
std::vector<Item> ReadItems(const std::vector<int>& ids, const Finished& finished) {
    std::vector<Item> items;
    Ref root(api.Invoke(achievementsRoot, nullptr));
    if (!root.get()) return items;
    for (const int id : ids) {
        int64_t key = id;
        Ref data(api.Invoke(achievementById, root.get(), {&key}));
        if (!data.get()) continue;
        Item item;
        item.id = id;
        item.points = api.Value<int>(api.Field(data.get(), "points"));
        item.level = api.Value<int>(api.Field(data.get(), "level"));
        Ref name(api.Invoke(achievementName, data.get())), description(api.Invoke(achievementDescription, data.get()));
        item.name = name.get() ? api.String(name.get()) : "Succès " + std::to_string(id);
        item.description = description.get() ? Plain(api.String(description.get())) : "";
        item.done = Contains(finished.a.get(), id) || Contains(finished.b.get(), id);
        items.push_back(std::move(item));
    }
    return items;
}

// The menu.
int HereMap() {
    Ref character(module->FindService("fng"));
    if (!character.get() || api.object_get_class(character.get()) != api.Class("Core.dll", "", "esg")) return 0;
    Ref location(api.Invoke(currentMap, character.get()));
    if (!location.get() || api.object_get_class(location.get()) != api.Class("Core.dll", "", "tk")) return 0;
    const int64_t map = *static_cast<int64_t*>(api.FieldAddress(location.get(), "m_mapId"));
    return map > 0 && map <= INT32_MAX ? static_cast<int>(map) : 0;
}
// The boss's achievements, then its dungeon's when this map is in the dungeon of that boss.
std::optional<Target> FindTarget(Obj group) {
    Ref root(api.Invoke(monstersRoot, nullptr));
    if (!root.get()) return std::nullopt;
    std::vector<Ref> monsters;
    monsters.emplace_back(api.Field(group, "dqyq"));
    Ref others(api.Field(group, "dqyr"));
    const int count = others.get() ? api.Value<int>(api.Call(others.get(), "get_Count")) : 0;
    if (count < 0 || count > 64) throw std::runtime_error("Boss achievements group bounds");
    for (int i = 0; i < count; ++i) monsters.emplace_back(api.Call(others.get(), "get_Item", {&i}));
    Target target;
    std::vector<std::vector<int>> lists;
    std::vector<int> bosses;
    for (const auto& monster : monsters) {
        if (!monster.get()) continue;
        int id = *static_cast<int*>(api.FieldAddress(monster.get(), "dcgq"));
        Ref data(api.Invoke(monsterById, root.get(), {&id}));
        if (!data.get() || !api.Value<bool>(api.Invoke(isBoss, data.get()))) continue;
        if (std::find(bosses.begin(), bosses.end(), id) != bosses.end()) continue;
        bosses.push_back(id);
        if (target.boss.empty()) { Ref name(api.Invoke(monsterName, data.get())); if (name.get()) target.boss = api.String(name.get()); }
        Ref list(api.Field(data.get(), "achievements"));
        lists.push_back(Numbers(list.get()));
    }
    if (bosses.empty()) return std::nullopt;
    size_t own = 0;
    for (const auto& list : lists) own += list.size();
    size_t fromDungeon = 0;
    if (int map = HereMap()) {
        Ref dungeons(api.Invoke(dungeonsRoot, nullptr));
        Ref dungeon(dungeons.get() ? api.Invoke(dungeonByMap, dungeons.get(), {&map}) : nullptr);
        if (dungeon.get()) {
            Ref listed(api.Field(dungeon.get(), "bosses"));
            const auto dungeonBosses = Numbers(listed.get());
            if (std::any_of(bosses.begin(), bosses.end(), [&](int id) { return std::find(dungeonBosses.begin(), dungeonBosses.end(), id) != dungeonBosses.end(); })) {
                Ref list(api.Field(dungeon.get(), "achievements"));
                lists.push_back(Numbers(list.get()));
                fromDungeon = lists.back().size();
            }
        }
    }
    target.ids = Merge(lists);
    Log("BOSS_ACHIEVEMENTS boss=" + std::to_string(bosses.front()) + " own=" + std::to_string(own) + " dungeon=" + std::to_string(fromDungeon) + " total=" + std::to_string(target.ids.size()));
    if (target.ids.empty()) return std::nullopt;
    return target;
}
void Extend(Obj menu, Obj group) {
    row.armed = false;
    if (!Enabled()) return;
    auto target = FindTarget(group);
    if (!target) return;
    const auto finished = ReadFinished();
    const auto sorted = Sort(ReadItems(target->ids, finished));
    const auto text = MenuLabel(target->boss, sorted, finished.known);
    Ref rows(api.Call(menu, "get_menuItems"));
    int index = api.Value<int>(api.Call(rows.get(), "get_Count"));
    Ref label(api.string_new(text.c_str())), help(api.string_new("Les succès de ce boss et de son donjon : ceux à faire et ceux déjà faits.")), empty(api.string_new(""));
    Ref asset(api.object_new(api.class_from_type(api.method_get_param(append, 7))));
    Ref color(api.object_new(api.class_from_type(api.method_get_param(append, 10))));
    bool disabled = false, close = true; unsigned delay = 1000; int style = 1, icon = IconFlag;
    api.Invoke(append, menu, {label.get(), nullptr, &disabled, &close, help.get(), &delay, empty.get(),
        api.object_unbox(asset.get()), &style, &icon, api.object_unbox(color.get()), empty.get(), empty.get()});
    row.item.reset(api.Call(rows.get(), "get_Item", {&index}));
    row.callback.reset(api.Call(row.item.get(), "get_m_callback"));
    row.target = std::move(*target);
    row.armed = row.callback.get() != nullptr;
}
bool NoPopulate(Obj, Obj) { return false; }
bool Clicked(Obj callback, Obj view) {
    if (!callback || callback != row.callback.get()) return false;
    try {
        if (!row.armed || !view || api.Call(view, "get_itemData") != row.item.get() || !Enabled()) return true;
        shown = row.target;
        wanted = true; newTarget = true;
    } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_ERROR ") + error.what()); }
    return true;
}

// The window.
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
void Show(Obj element, bool visible) { Set<int>(element, "display", visible ? 0 : 1); }
Obj Label(Obj parent, const std::string& text, void (*style)(Obj)) {
    Ref label(nativeStyle::NewLabel());
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
RowView& RowAt(std::vector<RowView>& rows, Obj box, size_t index) {
    while (rows.size() <= index) {
        RowView view;
        view.line.reset(Element(box));
        Pixels(view.line.get(), "marginBottom", 8);
        Ref head(Element(view.line.get()));
        Set<int>(head.get(), "flexDirection", 2);
        view.name.reset(Label(head.get(), "", nativeStyle::Strong));
        Set<float>(view.name.get(), "flexGrow", 1.f); Set<float>(view.name.get(), "flexShrink", 1.f);
        view.points.reset(Label(head.get(), "", nativeStyle::Secondary));
        Pixels(view.points.get(), "width", PointsWidth); Set<float>(view.points.get(), "flexShrink", 0.f);
        Set<int>(view.points.get(), "unityTextAlign", AlignRight);
        view.description.reset(Label(view.line.get(), "", nativeStyle::Secondary));
        rows.push_back(std::move(view));
    }
    return rows[index];
}
void Fill(std::vector<RowView>& rows, Obj box, const std::vector<Item>& items, bool done) {
    for (size_t i = 0; i < items.size(); ++i) {
        auto& view = RowAt(rows, box, i);
        Show(view.line.get(), true);
        Text(view.name.get(), items[i].name);
        nativeStyle::Tone(view.name.get(), done, false);
        Text(view.points.get(), std::to_string(items[i].points) + " pts");
        Text(view.description.get(), items[i].description);
        Show(view.description.get(), !items[i].description.empty());
    }
    for (size_t i = items.size(); i < rows.size(); ++i) Show(rows[i].line.get(), false);
}
void Refresh() {
    const auto finished = ReadFinished();
    const auto sorted = Sort(ReadItems(shown.ids, finished));
    Text(state.window.get(), "Succès : " + shown.boss, "set_headerTitle");
    Text(state.todoHead.get(), "À faire (" + std::to_string(sorted.todo.size()) + ")");
    Text(state.doneHead.get(), "Déjà faits (" + std::to_string(sorted.done.size()) + ")");
    Show(state.todoHead.get(), !sorted.todo.empty());
    Show(state.doneHead.get(), !sorted.done.empty());
    Fill(state.todo, state.todoBox.get(), sorted.todo, false);
    Fill(state.done, state.doneBox.get(), sorted.done, true);
    Show(state.status.get(), !finished.known);
}
bool Action(Obj owner) {
    if (!owner || owner != state.closeButton.get()) return false;
    wanted = false;
    return true;
}
Obj Root() {
    Ref hud(soul::GetUiRoot()); if (!hud.get()) return nullptr;
    Ref parent(api.Call(hud.get(), "get_parent"));
    Ref hierarchy(api.Call(parent.get(), "get_hierarchy"));
    const int count = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    if (count < 0 || count > 64) throw std::runtime_error("Boss achievements UI layer bounds");
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
    if (count < 0 || count > 32) throw std::runtime_error("Boss achievements native manipulator bounds");
    for (int i = 0; i < count; ++i) state.manipulators.emplace_back(api.Call(items.get(), "get_Item", {&i}));
}
void Create(Obj root) {
    state.root.reset(root); state.dependencies.reset(module->FindService("fpt"));
    if (!state.dependencies.get() || api.object_get_class(state.dependencies.get()) != api.Class("Core.dll", "", "goc"))
        throw std::runtime_error("Boss achievements native dependencies unavailable");
    state.cached.reset(api.Call(state.dependencies.get(), "bfri")); state.compositor.reset(module->compositor.get());
    Ref input(api.Call(state.dependencies.get(), "bfre"));
    if (state.cached.get() != module->cached.get() || !input.get() || api.object_get_class(input.get()) != api.Class("Core.dll", "", "eqo"))
        throw std::runtime_error("Boss achievements native session mismatch");
    state.window.reset(api.New(api.Class("Core.dll", "Core.UILogic.Components", "MagneticDragWindow")));
    Text(state.window.get(), "", "set_name"); Text(state.window.get(), "Succès", "set_headerTitle");
    bool yes = true; int icon = IconFlag, size = 1;
    api.Call(state.window.get(), "set_headerIcon", {&icon}); api.Call(state.window.get(), "set_headerSize", {&size});
    api.Call(state.window.get(), "set_headerShowCloseButton", {&yes});
    for (const auto* setter : {"set_verticalResizable", "set_horizontalResizable", "set_cornerResizable"})
        api.Call(state.window.get(), setter, {&yes});
    api.Call(state.window.get(), "set_hasBackground", {&yes});
    Set<int>(state.window.get(), "position", 1); Set<float>(state.window.get(), "flexGrow", 0.f);
    Pixels(state.window.get(), "width", 400); Pixels(state.window.get(), "height", 420);
    Pixels(state.window.get(), "minWidth", 280); Pixels(state.window.get(), "minHeight", 180);
    Pixels(state.window.get(), "left", 200); Pixels(state.window.get(), "top", 140);
    state.header.reset(api.Call(state.window.get(), "get_headerWidget"));
    state.closeButton.reset(api.Field(state.header.get(), "closeButton"));
    if (!state.closeButton.get()) throw std::runtime_error("Boss achievements native close button unavailable");
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
    state.status.reset(Label(content.get(), "Le jeu n’a pas encore donné tes succès terminés : ouvre une fois la fenêtre des succès.", nativeStyle::Secondary));
    Pixels(state.status.get(), "marginBottom", 8);
    state.todoHead.reset(Label(content.get(), "", nativeStyle::Heading));
    Pixels(state.todoHead.get(), "marginBottom", 6);
    state.todoBox.reset(Element(content.get()));
    state.doneHead.reset(Label(content.get(), "", nativeStyle::Heading));
    Pixels(state.doneHead.get(), "marginTop", 4); Pixels(state.doneHead.get(), "marginBottom", 6);
    state.doneBox.reset(Element(content.get()));
    Refresh();
    Add(root, state.window.get());
    Log("BOSS_ACHIEVEMENTS_OPEN items=" + std::to_string(shown.ids.size()));
}
}

void InstallBossAchievements() {
    try {
        const auto data = api.Class(DataImage, "Core.DataCenter", "DataCenterModule");
        achievementsRoot = api.FindVerified(data, "get_achievementsDataRoot", 0, 0x7B1270);
        monstersRoot = api.FindVerified(data, "get_monstersDataRoot", 0, 0x7B6270);
        dungeonsRoot = api.FindVerified(data, "get_dungeonsDataRoot", 0, 0x7B3610);
        achievementById = api.FindVerified(api.Class(DataImage, QuestSpace, "AchievementsDataRoot"), "GetAchievementById", 1, 0x7AEF30);
        const auto achievement = api.Class(DataImage, QuestSpace, "AchievementData");
        Field(achievement, "points", 36); Field(achievement, "level", 40);
        achievementName = api.FindVerified(achievement, "get_name", 0, 0x7AE6F0);
        achievementDescription = api.FindVerified(achievement, "get_description", 0, 0x7AE770);
        monsterById = api.FindVerified(api.Class(DataImage, "Core.DataCenter.Metadata.Monster", "MonstersDataRoot"), "GetMonsterById", 1, 0x7DF5F0);
        const auto monster = api.Class(DataImage, "Core.DataCenter.Metadata.Monster", "MonsterData");
        Field(monster, "achievements", 176);
        isBoss = api.FindVerified(monster, "get_isBoss", 0, 0x7BB210);
        monsterName = api.FindVerified(monster, "get_name", 0, 0x7DE880);
        dungeonByMap = api.FindVerified(api.Class(DataImage, "Core.DataCenter.Metadata.World", "DungeonsDataRoot"), "GetDungeonByMapId", 1, 0x7B9500);
        const auto dungeon = api.Class(DataImage, "Core.DataCenter.Metadata.World", "DungeonData");
        Field(dungeon, "achievements", 72); Field(dungeon, "bosses", 80);
        serviceClass = api.Class("Core.dll", "", "fcx");
        finishedA = api.FindVerified(serviceClass, "bcwx", 0, 0x69A650);
        finishedB = api.FindVerified(serviceClass, "bcxd", 0, 0x73FA90);
        currentMap = api.FindVerified(api.Class("Core.dll", "", "esg"), "zeo", 0, 0x1854C30);
        append = api.FindVerified(api.Class("Core.dll", "Core.UILogic.Components.ContextMenu", "DofusContextualMenu"), "AppendDefaultItem", 13, 0x16D06F0);
        unsubscribe = api.FindVerified(api.Class("Core.dll", "Core.Engine.Inputs.Events", "InputCustomEvent"), "UnSubscribe", 1, 0x2097300);
        soul::SetAuxiliaryPanelMenus(NoPopulate, Clicked);
        soul::SetAuxiliaryPanelAction(Action); nativeClick::AddHandler(Action);
        toolsUi::Register(Key, "Succès du boss", IconFlag, [] { return wanted; }, [] { wanted = !wanted && !shown.ids.empty(); return true; }, {},
            [] { return false; });
        SetSoulGroupMenuExtension(Extend);
        installed = true;
        Log("BOSS_ACHIEVEMENTS_READY");
    } catch (const std::exception& error) {
        Log(std::string("BOSS_ACHIEVEMENTS_UNAVAILABLE ") + error.what());
    }
}

void RestoreBossAchievements() {
    wanted = false; newTarget = false; row.armed = false;
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
            if (count < 0 || count > 16384) throw std::runtime_error("Boss achievements input subscriber bounds");
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
    } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_CLEANUP_ERROR ") + error.what()); }
}
bool BossAchievementsCleanupPending() { return state.cleanup; }
void CloseBossAchievementsOffWorld() { if (installed && (wanted || state.window.get())) RestoreBossAchievements(); }

void TickBossAchievements() {
    if (!installed) return;
    if (!wanted || !Session() || state.cleanup || (state.window.get() && !Current())) {
        if (wanted || state.window.get() || state.cleanup) RestoreBossAchievements();
        return;
    }
    if (!state.window.get()) {
        Ref root(Root()); if (!root.get() || !api.Call(root.get(), "get_panel")) return;
        try { Create(root.get()); newTarget = false; state.nextRefresh = GetTickCount64() + RefreshMs; }
        catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_CREATE_ERROR ") + error.what()); RestoreBossAchievements(); }
        return;
    }
    try {
        const auto now = GetTickCount64();
        if (newTarget || now >= state.nextRefresh) {
            newTarget = false; state.nextRefresh = now + RefreshMs;
            Refresh();
        }
    } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_ERROR ") + error.what()); RestoreBossAchievements(); }
}
