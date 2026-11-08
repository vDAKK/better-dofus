#include "ui/monster-target.hpp"
#include "ui/monster-target-policy.hpp"
#include "glow/monster-glow-entities.hpp"
#include "core/module.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using namespace managed;
using namespace monsterTarget;
constexpr const char* Ui = "UnityEngine.UIElementsModule.dll";
constexpr const char* UiSpace = "UnityEngine.UIElements";
constexpr const char* Engine = "UnityEngine.CoreModule.dll";
constexpr const char* Figma = "Core.UILogic.Components.Figma";
constexpr int IconArchimonster = 10; // FigmaIcons.archimonster, the game's own badge.
constexpr float Violet[4] = {0.72f, 0.42f, 1.f, 1.f};
constexpr uint64_t ScanMs = 300;
constexpr size_t MaxBadges = 32;
// BestiaryTabUI: its binding (a struct at 16) holds the monster card and its header.
constexpr size_t TabDetails = 240, TabInfo = 248, WrapperData = 32, MonsterId = 20;
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Bounds { Vec3 center, extents; };
struct Group { int64_t id = 0; std::vector<Ref> renderers; };
struct State {
    Ref checkbox, details;
    int monster = 0;
    bool shown = false;
    Ref overlay, root;
    std::vector<Ref> badges;
    std::vector<Group> groups;
    uint64_t nextScan = 0;
    bool cleanupPending = false;
} state;
using RefreshFn = void (*)(Obj, Obj, Method);
RefreshFn originalRefresh = nullptr;
Method rootGetter{}, toPanel{}, toLocal{}, cameraGetter{}, toScreen{}, exists{}, groupStore{}, iconSet{}, iconColor{};
Klass checkboxClass{}, iconClass{}, cameraServiceClass{}, layerServiceClass{}, layerRootClass{}, storeServiceClass{}, visualClass{};
bool installed = false, reported = false;

bool Session() { return installed && module && module->ready && module->servicesReady && !module->stopped && !module->stopRequested; }
void Report(const char* where, const std::exception& error) {
    if (!reported) Log(std::string("MONSTER_TARGET_ERROR ") + where + ": " + error.what());
    reported = true;
}
void Field(Klass type, const char* name, size_t offset) {
    const auto field = api.class_get_field_from_name(type, name);
    if (!field || api.field_get_offset(field) != offset) throw std::runtime_error(std::string("Monster target field layout: ") + name);
}
Obj ReadStyle(Obj element, const char* name) {
    Ref style(api.Call(element, "get_style"));
    return api.Call(style.get(), (std::string("UnityEngine.UIElements.IStyle.get_") + name).c_str());
}
void WriteStyle(Obj element, const char* name, Obj value) {
    Ref style(api.Call(element, "get_style"));
    api.Call(style.get(), (std::string("UnityEngine.UIElements.IStyle.set_") + name).c_str(), {api.object_unbox(value)});
}
void EnumStyle(Obj element, const char* name, int value) {
    Ref box(ReadStyle(element, name)); api.Call(box.get(), "set_value", {&value}); WriteStyle(element, name, box.get());
}
void Pixels(Obj element, const char* name, float value) {
    Ref box(ReadStyle(element, name)); int unit = 0;
    Ref length(api.New(api.Class(Ui, UiSpace, "Length"), {&value, &unit}));
    api.Call(box.get(), "set_value", {api.object_unbox(length.get())}); WriteStyle(element, name, box.get());
}
void Passive(Obj element) {
    int ignore = 1; api.Call(element, "set_pickingMode", {&ignore});
    EnumStyle(element, "position", 1);
}
void Detach(Ref& element) {
    if (element.get() && api.Call(element.get(), "get_parent")) api.Call(element.get(), "RemoveFromHierarchy");
    element.reset();
}

// The bestiary: a checkbox under the monster card's header, ticked for a target.
int SelectedMonster(Obj wrapper) {
    if (!wrapper) return 0;
    const auto data = Read<Obj>(wrapper, WrapperData);
    return data ? Read<uint16_t>(data, MonsterId) : 0;
}
void Decorate(Obj tab, Obj wrapper) {
    const auto details = Read<Obj>(tab, TabDetails), info = Read<Obj>(tab, TabInfo);
    if (!details || !info || !api.class_is_assignable_from(visualClass, api.object_get_class(details)) ||
        api.Call(info, "get_parent") != details) return;
    const int monster = SelectedMonster(wrapper);
    if (monster <= 0) return;
    if (!state.checkbox.get() || api.Call(state.checkbox.get(), "get_parent") != details) {
        Detach(state.checkbox);
        state.checkbox.reset(api.New(checkboxClass));
        Ref text(api.string_new("Cibler ce monstre : icône violette au-dessus de ses groupes"));
        api.Call(state.checkbox.get(), "set_rightText", {text.get()});
        for (const char* side : {"marginLeft", "marginTop", "marginBottom"}) Pixels(state.checkbox.get(), side, 8);
        int index = api.Value<int>(api.Call(details, "IndexOf", {info})) + 1;
        api.Call(details, "Insert", {&index, state.checkbox.get()});
        state.details.reset(details);
    }
    state.monster = monster;
    bool value = state.shown = Contains(module->settings.targetMonsters, monster);
    api.Call(state.checkbox.get(), "set_value", {&value});
}
void RefreshHook(Obj tab, Obj wrapper, Method method) {
    originalRefresh(tab, wrapper, method);
    if (!tab || !Session()) return;
    try { Decorate(tab, wrapper); } catch (const std::exception& error) { Report("bestiary", error); }
}
void PollCheckbox() {
    if (!state.checkbox.get() || state.monster <= 0 || !api.Call(state.checkbox.get(), "get_panel")) return;
    const bool value = api.Value<bool>(api.Call(state.checkbox.get(), "get_value"));
    if (value == state.shown) return;
    state.shown = value;
    auto next = module->settings;
    next.targetMonsters = Toggle(next.targetMonsters, state.monster, value);
    if (next.targetMonsters == module->settings.targetMonsters) return;
    module->Change(next);
    state.nextScan = 0;
    Log("MONSTER_TARGET " + std::string(value ? "ADD " : "REMOVE ") + std::to_string(state.monster) +
        " count=" + std::to_string(next.targetMonsters.size()));
}

// The map: the groups of a targeted monster, read every ScanMs, then their badge each frame.
std::vector<int> GroupMonsters(Obj group) {
    std::vector<int> ids;
    if (const auto main = api.Field(group, "dqyq")) ids.push_back(*static_cast<int*>(api.FieldAddress(main, "dcgq")));
    Ref others(api.Field(group, "dqyr"));
    const int count = others.get() ? api.Value<int>(api.Call(others.get(), "get_Count")) : 0;
    if (count < 0 || count > 64) throw std::runtime_error("Monster target group bounds");
    for (int i = 0; i < count; ++i) {
        Ref monster(api.Call(others.get(), "get_Item", {&i}));
        if (monster.get()) ids.push_back(*static_cast<int*>(api.FieldAddress(monster.get(), "dcgq")));
    }
    return ids;
}
void Scan() {
    state.groups.clear();
    Ref statics(module->FindService("fsr"));
    if (!statics.get() || api.object_get_class(statics.get()) != storeServiceClass) return;
    Ref store(api.Invoke(groupStore, statics.get()));
    if (!store.get()) return;
    const auto find = api.FindVerified(api.object_get_class(store.get()), "baqa", 2, 0x3BBE660);
    auto snapshot = monsterGlow::ReadMonsterGroups();
    if (!snapshot.valid) return;
    for (auto& target : snapshot.targets) {
        if (state.groups.size() >= MaxBadges) break;
        Obj group = nullptr;
        int64_t id = target.id;
        if (!api.Value<bool>(api.Invoke(find, store.get(), {&id, &group})) || !group) continue;
        if (!Targeted(module->settings.targetMonsters, GroupMonsters(group))) continue;
        state.groups.push_back({id, std::move(target.renderers)});
    }
}
Obj HudRoot() {
    Ref document(api.Invoke(rootGetter, nullptr));
    Ref layerService(module->FindService("fqp"));
    if (!document.get() || !layerService.get() || api.object_get_class(layerService.get()) != layerServiceClass) return nullptr;
    Ref layers(api.Field(layerService.get(), "dzrv"));
    if (!layers.get() || api.object_get_class(layers.get()) != layerRootClass || api.Call(layers.get(), "get_parent") != document.get()) return nullptr;
    // Below the windows, menus and tooltips, as the glow's contours.
    Ref hierarchy(api.Call(layers.get(), "get_hierarchy"));
    const int count = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    for (int i = 0; i < count && i < 64; ++i) {
        Ref child(api.Call(hierarchy.get(), "get_Item", {&i}));
        if (child.get() && api.String(api.Call(child.get(), "get_name")) == "RootLow") return child.get();
    }
    return nullptr;
}
void HideBadges(size_t from = 0) {
    for (size_t i = from; i < state.badges.size(); ++i) EnumStyle(state.badges[i].get(), "display", 1);
}
// The top centre of the group's sprites, in the HUD root's coordinates.
bool Anchor(const Group& group, Obj camera, Obj panel, Obj root, Vec2& point) {
    float left = std::numeric_limits<float>::infinity(), right = -left, top = -left, depth = 0;
    for (const auto& renderer : group.renderers) {
        if (!renderer.get() || !api.Value<bool>(api.Invoke(exists, nullptr, {renderer.get()}))) return false;
        const auto bounds = api.Value<Bounds>(api.Call(renderer.get(), "get_bounds"));
        left = std::min(left, bounds.center.x - bounds.extents.x); right = std::max(right, bounds.center.x + bounds.extents.x);
        top = std::max(top, bounds.center.y + bounds.extents.y); depth = bounds.center.z;
    }
    if (!(right >= left) || !std::isfinite(top)) return false;
    Vec3 world{(left + right) / 2, top, depth};
    const auto screen = api.Value<Vec3>(api.Invoke(toScreen, camera, {&world}));
    if (!std::isfinite(screen.x) || !std::isfinite(screen.y) || screen.z <= 0) return false;
    Vec2 flat{screen.x, screen.y};
    const auto onPanel = api.Value<Vec2>(api.Invoke(toPanel, nullptr, {&flat, panel}));
    point = api.Value<Vec2>(api.Invoke(toLocal, nullptr, {root, const_cast<Vec2*>(&onPanel)}));
    return std::isfinite(point.x) && std::isfinite(point.y);
}
void DrawBadges() {
    Ref root(HudRoot());
    Ref cameraService(module->FindService("fpk"));
    if (!root.get() || !cameraService.get() || !api.class_is_assignable_from(cameraServiceClass, api.object_get_class(cameraService.get()))) { HideBadges(); return; }
    Ref camera(api.Invoke(cameraGetter, cameraService.get()));
    Ref panel(api.Call(root.get(), "get_panel"));
    if (!camera.get() || !panel.get()) { HideBadges(); return; }
    if (!state.overlay.get() || state.root.get() != root.get() || api.Call(state.overlay.get(), "get_parent") != root.get()) {
        Detach(state.overlay); state.badges.clear();
        state.overlay.reset(api.New(visualClass)); Passive(state.overlay.get());
        Pixels(state.overlay.get(), "left", 0); Pixels(state.overlay.get(), "top", 0);
        api.Call(root.get(), "Add", {state.overlay.get()});
        state.root.reset(root.get());
    }
    size_t shown = 0;
    for (const auto& group : state.groups) {
        Vec2 point{};
        if (!Anchor(group, camera.get(), panel.get(), root.get(), point)) continue;
        if (shown == state.badges.size()) {
            Ref icon(api.New(iconClass));
            int figma = IconArchimonster; api.Invoke(iconSet, icon.get(), {&figma});
            float color[4] = {Violet[0], Violet[1], Violet[2], Violet[3]}; api.Invoke(iconColor, icon.get(), {color});
            Passive(icon.get());
            Pixels(icon.get(), "width", BadgeSize); Pixels(icon.get(), "height", BadgeSize);
            api.Call(state.overlay.get(), "Add", {icon.get()});
            state.badges.push_back(std::move(icon));
        }
        const auto rect = Badge(point.x, point.y);
        const auto badge = state.badges[shown++].get();
        Pixels(badge, "left", rect.x); Pixels(badge, "top", rect.y);
        EnumStyle(badge, "display", 0);
    }
    HideBadges(shown);
}
}

void InstallMonsterTarget() {
    if (installed) return;
    try {
        monsterGlow::InstallEntities();
        visualClass = api.Class(Ui, UiSpace, "VisualElement");
        api.FindVerified(visualClass, "Insert", 2, 0x55AAA50); api.FindVerified(visualClass, "IndexOf", 1, 0x55AA8F0);
        api.FindVerified(visualClass, "Add", 1, 0x55A4E10); api.FindVerified(visualClass, "RemoveFromHierarchy", 0, 0x55ACF10);
        api.FindVerified(visualClass, "set_pickingMode", 1, 0x55BEAF0); api.FindVerified(visualClass, "get_parent", 0, 0x55BC4C0);
        api.FindVerified(visualClass, "get_panel", 0, 0x55BC4B0); api.FindVerified(visualClass, "get_hierarchy", 0, 0x1854EF0);
        api.FindVerified(visualClass, "get_name", 0, 0x311BA90);
        const auto hierarchy = api.NestedClass(visualClass, "Hierarchy");
        api.FindVerified(hierarchy, "get_childCount", 0, 0x55C4DC0); api.FindVerified(hierarchy, "get_Item", 1, 0x55C39B0);
        checkboxClass = api.Class("Core.dll", Figma, "CheckboxCustom");
        api.FindVerified(checkboxClass, "get_value", 0, 0x16FEAE0); api.FindVerified(checkboxClass, "set_value", 1, 0x16FF100);
        api.FindVerified(checkboxClass, "set_rightText", 1, 0x16FF020);
        iconClass = api.Class("Core.dll", Figma, "DofusIcon");
        api.FindVerified(iconClass, ".ctor", 0, 0x1708660);
        iconSet = api.FindVerified(iconClass, "set_icon", 1, 0x1708F10);
        iconColor = api.FindVerified(iconClass, "set_color", 1, 0x1708CC0);
        const auto utility = api.Class("Core.dll", "", "pq");
        rootGetter = api.FindVerified(utility, "pbs", 0, 0x2E57130);
        toPanel = api.FindVerified(utility, "pcf", 2, 0x2E58080);
        toLocal = api.FindVerified(api.Class(Ui, UiSpace, "VisualElementExtensions"), "WorldToLocal", 2, 0x563C9E0);
        cameraServiceClass = api.Class("Core.dll", "", "eow");
        cameraGetter = api.FindVerified(cameraServiceClass, "xhs", 0, 0x68D2C0);
        toScreen = api.FindVerified(api.Class(Engine, "UnityEngine", "Camera"), "WorldToScreenPoint", 1, 0x537B8F0);
        api.FindVerified(api.Class(Engine, "UnityEngine", "Renderer"), "get_bounds", 0, 0x53B7560);
        exists = api.FindVerified(api.Class(Engine, "UnityEngine", "Object"), "op_Implicit", 1, 0x53FE4D0);
        layerServiceClass = api.Class("Core.dll", "", "gmo");
        layerRootClass = api.Class("Core.dll", "", "gmy");
        storeServiceClass = api.Class("Core.dll", "", "ewa");
        groupStore = api.FindVerified(storeServiceClass, "bapo", 0, 0x73BCA0);
        Field(api.Class("Core.dll", "", "ewd"), "dqyq", 16); Field(api.Class("Core.dll", "", "ewd"), "dqyr", 24);
        Field(api.Class("Core.dll", "", "fh"), "dcgq", 16);
        const auto bindings = api.Class("Core.dll", "Core.UILogic.Bindings", "BestiaryTabBinding");
        Field(bindings, "_ctr_monsterDetails", TabDetails); Field(bindings, "_ctr_monsterInfo", TabInfo);
        Field(api.Class("Core.dll", "Core.UILogic.Bestiary", "MonsterInfoWrapper"), "data", WrapperData);
        Field(api.Class("Ankama.Dofus.Core.DataCenter.dll", "Core.DataCenter.Metadata.Monster", "MonsterData"), "id", MonsterId);
        const auto tab = api.Class("Core.dll", "Core.UILogic.Bestiary", "BestiaryTabUI");
        AddHook(api.FindVerified(tab, "RefreshMonsterDetails", 1, 0x1564270), 0x1564270, reinterpret_cast<void*>(&RefreshHook), reinterpret_cast<void**>(&originalRefresh));
        installed = true;
        Log("MONSTER_TARGET_READY targets=" + std::to_string(module ? module->settings.targetMonsters.size() : 0));
    } catch (const std::exception& error) { Log(std::string("MONSTER_TARGET_UNAVAILABLE ") + error.what()); }
}
void TickMonsterTarget() {
    if (!Session()) return;
    try {
        PollCheckbox();
        if (module->settings.targetMonsters.empty() || module->InCombat()) {
            state.groups.clear(); if (!state.badges.empty()) HideBadges(); return;
        }
        const auto now = GetTickCount64();
        if (now >= state.nextScan) { state.nextScan = now + ScanMs; Scan(); }
        if (state.groups.empty()) { HideBadges(); return; }
        DrawBadges();
    } catch (const std::exception& error) {
        Report("tick", error);
        state.groups.clear();
        try { HideBadges(); } catch (...) {}
    }
}
void RestoreMonsterTarget() {
    state.cleanupPending = false;
    try {
        Detach(state.checkbox); state.details.reset(); state.monster = 0;
        Detach(state.overlay); state.badges.clear(); state.groups.clear(); state.root.reset();
    } catch (const std::exception& error) { Report("restore", error); state.cleanupPending = true; }
}
bool MonsterTargetCleanupPending() { return state.cleanupPending; }
