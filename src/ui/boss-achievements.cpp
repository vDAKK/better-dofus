#include "ui/boss-achievements.hpp"
#include "ui/boss-achievements-policy.hpp"
#include "ui/item-search-policy.hpp"
#include "ui/native-click.hpp"
#include "ui/native-style.hpp"
#include "ui/tools-ui.hpp"
#include "core/module.hpp"
#include "maps/resource-map-icons.hpp"
#include "souls/soul-combat.hpp"
#include "souls/soul-group-menu.hpp"
#include "souls/soul-panel.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

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
constexpr uint64_t RefreshMs = 2000, FightRetryMs = 1000;

struct RowView { Ref line, card, name, points, description, icon, objectivesBox; std::vector<Ref> objectives; int shownIcon = 0, achievement = 0; bool clickable = false; };
struct State {
    Ref root, window, header, dragger, dependencies, cached, compositor, closeButton;
    Ref status, todoHead, todoBox, doneHead, doneBox, showDone;
    std::array<Ref, 4> inputEvents;
    std::vector<Ref> manipulators;
    std::vector<RowView> todo, done;
    uint64_t nextRefresh = 0;
    bool cleanup = false, doneShown = true;
} state;
struct Target { std::string boss; std::vector<int> ids; };
Target shown; // What the window shows.
struct MenuRow { Ref item, callback; Target target; bool armed = false; } row;

Method achievementsRoot = nullptr, monstersRoot = nullptr, dungeonsRoot = nullptr, achievementById = nullptr, monsterById = nullptr, dungeonByMap = nullptr;
Method isBoss = nullptr, monsterName = nullptr, achievementName = nullptr, achievementDescription = nullptr, finishedAccount = nullptr, finishedCharacter = nullptr;
Method currentMap = nullptr, append = nullptr, unsubscribe = nullptr;
Method categoryOf = nullptr, categoryVisible = nullptr, settingsOf = nullptr, cardAsset = nullptr, cloneTree = nullptr, doneClassOf = nullptr, loadIcon = nullptr;
void* iconTypeField = nullptr;
std::string doneClass;
std::set<int> hiddenLogged;
// The window as opened from a fight: only what is left at first, closed with the fight.
bool fightWindow = false;
uint64_t nextFightRead = 0;
// The icons of the fight's challenge panel (UIChallengeSelectionDisplay).
constexpr size_t SuccessListOffset = 0x10 + 72 - 16; // Its binding's field, in the object.
constexpr size_t MaxPanelIcons = 6;
constexpr int TipTooltipLeft = 3, TipSourceRight = 5, TipLeave = 0;
// nativeHidden: the game has no achievement to show. It hides the list without emptying
// it: what is left there comes from an earlier fight.
struct Panel { Ref display; std::vector<Ref> icons; std::vector<int> enemies; Target target; bool dirty = false, nativeHidden = false; } panel;
using DisplayFn = void (*)(Obj, Method);
DisplayFn originalDisplay = nullptr;
Method updateIcon = nullptr, addManipulator = nullptr, clickableCtor = nullptr, tooltipCtor = nullptr, objectivesRoot = nullptr, objectiveName = nullptr;
Klass iconClass = nullptr, clickableClass = nullptr, tooltipClass = nullptr;
// What is known of each achievement's objectives: its data, and the game's answer. The
// game only learns these from the server, as when its own window opens an achievement.
// Hovering asks at most once in ten minutes per achievement; a fight's end forgets them.
constexpr uint64_t RequestSpacingMs = 400, AnswerLifeMs = 600000, RetryMs = 10000;
struct Answer { std::set<int> finished; uint64_t at = 0; };
std::map<int, std::vector<Objective>> objectiveData;
std::map<int, Answer> answers;
std::map<int, uint64_t> asked;
std::vector<int> toAsk;
uint64_t nextAsk = 0;
bool answersUnavailable = false;
Ref detailsSender, detailsConnection;
Method sendDetails = nullptr, answerOf = nullptr, linkCtor = nullptr, linkOpen = nullptr, createArray = nullptr, setArray = nullptr;
Klass senderClass = nullptr, requestClass = nullptr, linkClass = nullptr;
Ref stringType;
struct NullableColor { bool hasValue = false; float r = 0, g = 0, b = 0, a = 0; };
struct Vec2 { float x = 0, y = 0; };
struct TooltipView { Ref builder; bool dirty = false; } tooltipView;
using SetupFn = void (*)(Obj, Method);
SetupFn originalTooltipSetup = nullptr;
Method questObjectivesRoot = nullptr, questObjectiveById = nullptr, questsRoot = nullptr, questById = nullptr, questName = nullptr, objectiveText = nullptr, objectiveStep = nullptr, iconCtor = nullptr, iconSet = nullptr, iconColor = nullptr;
Klass questServiceClass = nullptr, statusIconClass = nullptr;
struct ActiveObjective { std::vector<int> parameters; std::string text, folded, quest; };
std::map<int, ActiveObjective> activeObjectives;
bool tooltipReported = false, wasInFight = false;
constexpr const char* TooltipBlockName = "better_dofus_monster_block";
constexpr int IconTick = 307, IconTrophy = 320, IconExclamation = 130;
using AnswerFn = void (*)(Obj, Obj, Method);
std::array<AnswerFn, 2> originalAnswer{};
// fcv's handler of a finished achievement always adds it to the account's set, and to
// the character's set only when it was achieved by the character in play.
Klass serviceClass = nullptr;
bool installed = false, wanted = false, newTarget = false;

void Field(Klass type, const char* name, size_t offset) {
    const auto field = api.class_get_field_from_name(type, name);
    if (!field || api.field_get_offset(field) != offset) throw std::runtime_error(std::string("Boss achievements field layout: ") + name);
}
bool Session() { return module && module->ready && module->servicesReady && !module->stopped && !module->stopRequested; }
bool Enabled() { return Session() && module->settings.bossAchievements; }
bool TooltipEnabled() { return Session() && module->settings.monsterTooltip; }
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
// What this character has finished. The account's set only tells that the game has
// received the lists: a character without any achievement has an empty set of its own.
struct Finished { Ref character; bool known = false; };
Finished ReadFinished() {
    Finished finished;
    Obj service = module->FindService("fja");
    if (!service || api.object_get_class(service) != serviceClass) return finished;
    Ref account(api.Invoke(finishedAccount, service));
    finished.character.reset(api.Invoke(finishedCharacter, service));
    const auto count = [](Obj set) { return set ? api.Value<int>(api.Call(set, "get_Count")) : 0; };
    finished.known = count(account.get()) + count(finished.character.get()) > 0;
    return finished;
}
// As the game before it shows an achievement: its category's visibility for this
// character (LobbyTab.SetMainInformations). When that cannot be read, it stays.
bool Visible(Obj data, int id) {
    try {
        int category = api.Value<int>(api.Field(data, "categoryId"));
        Ref wrapper(api.Invoke(categoryOf, nullptr, {&category}));
        Ref provider(module->FindService("fkq"));
        if (!wrapper.get() || !provider.get()) return true;
        const bool visible = api.Value<bool>(api.Invoke(categoryVisible, wrapper.get(), {provider.get()}));
        if (!visible && hiddenLogged.insert(id).second) Log("BOSS_ACHIEVEMENTS_HIDDEN id=" + std::to_string(id) + " category=" + std::to_string(category));
        return visible;
    } catch (const std::exception& error) {
        if (hiddenLogged.insert(-id).second) Log(std::string("BOSS_ACHIEVEMENTS_VISIBILITY_ERROR ") + error.what());
        return true;
    }
}
bool Contains(Obj set, int id) { return set && api.Value<bool>(api.Call(set, "Contains", {&id})); }
std::vector<Item> ReadItems(const std::vector<int>& ids, const Finished& finished) {
    std::vector<Item> items;
    Ref root(api.Invoke(achievementsRoot, nullptr));
    if (!root.get()) return items;
    for (const int id : ids) {
        int64_t key = id;
        Ref data(api.Invoke(achievementById, root.get(), {&key}));
        if (!data.get() || !Visible(data.get(), id)) continue;
        Item item;
        item.id = id;
        item.points = api.Value<int>(api.Field(data.get(), "points"));
        item.level = api.Value<int>(api.Field(data.get(), "level"));
        item.icon = api.Value<int>(api.Field(data.get(), "iconId"));
        Ref name(api.Invoke(achievementName, data.get())), description(api.Invoke(achievementDescription, data.get()));
        item.name = name.get() ? api.String(name.get()) : "Succès " + std::to_string(id);
        item.description = description.get() ? Plain(api.String(description.get())) : "";
        item.done = Contains(finished.character.get(), id);
        items.push_back(std::move(item));
    }
    return items;
}

const std::vector<Objective>& ObjectivesOf(int id) {
    const auto found = objectiveData.find(id);
    if (found != objectiveData.end()) return found->second;
    std::vector<Objective> result;
    try {
        Ref root(api.Invoke(achievementsRoot, nullptr));
        int64_t key = id;
        Ref data(root.get() ? api.Invoke(achievementById, root.get(), {&key}) : nullptr);
        Ref objectives(api.Invoke(objectivesRoot, nullptr));
        if (data.get() && objectives.get()) {
            Ref list(api.Field(data.get(), "objectiveIds"));
            for (const int objectiveId : Numbers(list.get())) {
                int64_t objectiveKey = objectiveId;
                Ref objective(api.Call(objectives.get(), "GetObjectById", {&objectiveKey}));
                if (!objective.get()) continue;
                Ref name(api.Invoke(objectiveName, objective.get())), criterion(api.Field(objective.get(), "criterion"));
                result.push_back({objectiveId, criterion.get() ? MonsterOf(api.String(criterion.get())) : 0, name.get() ? Plain(api.String(name.get())) : ""});
            }
        }
    } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_OBJECTIVE_ERROR ") + error.what()); }
    return objectiveData.emplace(id, std::move(result)).first->second;
}
// Asks at most once in a while, one at a time: the window's or the fight's achievements.
void Ask(int id) {
    if (answersUnavailable || id <= 0 || id > 32767) return;
    const auto now = GetTickCount64();
    const auto answer = answers.find(id);
    if (answer != answers.end() && now - answer->second.at < AnswerLifeMs) return;
    const auto before = asked.find(id);
    if (before != asked.end() && now - before->second < RetryMs) return;
    if (std::find(toAsk.begin(), toAsk.end(), id) == toAsk.end()) toAsk.push_back(id);
}
void PumpQuestions() {
    const auto now = GetTickCount64();
    if (toAsk.empty() || now < nextAsk) return;
    nextAsk = now + RequestSpacingMs;
    const int id = toAsk.front();
    toAsk.erase(toAsk.begin());
    asked[id] = now;
    Ref connection(module->FindService("fkm")), bus(module->FindService("fmz")), service(module->FindService("fja"));
    if (!connection.get() || !bus.get() || !service.get() || api.object_get_class(service.get()) != serviceClass) {
        answersUnavailable = true; toAsk.clear(); panel.dirty = true;
        Log("BOSS_ACHIEVEMENTS_DETAILS_UNAVAILABLE");
        return;
    }
    if (detailsConnection.get() != connection.get() || !detailsSender.get()) {
        detailsConnection.reset(connection.get());
        detailsSender.reset(api.New(senderClass, {bus.get(), connection.get(), service.get()}));
    }
    short key = static_cast<short>(id);
    Ref request(api.New(requestClass, {&key}));
    api.Invoke(sendDetails, detailsSender.get(), {request.get()});
}
template <size_t Index> void AnswerHook(Obj self, Obj message, Method method) {
    originalAnswer[Index](self, message, method);
    try {
        Ref achievement(api.Field(message, "fgtm"));
        Ref details(achievement.get() ? api.Invoke(answerOf, nullptr, {achievement.get()}) : nullptr);
        if (!details.get()) return;
        const int id = api.Value<int>(api.Field(details.get(), "dcmb"));
        Answer answer;
        answer.at = GetTickCount64();
        Ref finished(api.Field(details.get(), "dcmc"));
        const int count = finished.get() ? api.Value<int>(api.Call(finished.get(), "get_Count")) : 0;
        for (int i = 0; i < count && i < 512; ++i) {
            Ref objective(api.Call(finished.get(), "get_Item", {&i}));
            if (objective.get()) answer.finished.insert(api.Value<int>(api.Field(objective.get(), "dcly")));
        }
        Log("BOSS_ACHIEVEMENTS_DETAILS id=" + std::to_string(id) + " finished=" + std::to_string(answer.finished.size()));
        answers[id] = std::move(answer);
        panel.dirty = true; state.nextRefresh = 0; tooltipView.dirty = true;
    } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_DETAILS_ERROR ") + error.what()); }
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
// The group's monsters.
std::vector<int> GroupMonsters(Obj group) {
    std::vector<Ref> monsters;
    monsters.emplace_back(api.Field(group, "dqyq"));
    Ref others(api.Field(group, "dqyr"));
    const int count = others.get() ? api.Value<int>(api.Call(others.get(), "get_Count")) : 0;
    if (count < 0 || count > 64) throw std::runtime_error("Boss achievements group bounds");
    for (int i = 0; i < count; ++i) monsters.emplace_back(api.Call(others.get(), "get_Item", {&i}));
    std::vector<int> ids;
    for (const auto& monster : monsters)
        if (monster.get()) ids.push_back(*static_cast<int*>(api.FieldAddress(monster.get(), "dcgq")));
    return ids;
}
// The monsters' achievements, then the dungeon's when this map is in the dungeon of a
// boss among them.
std::optional<Target> FromMonsters(const std::vector<int>& monsterIds, bool quiet = false) {
    Ref root(api.Invoke(monstersRoot, nullptr));
    if (!root.get()) return std::nullopt;
    Target target;
    std::vector<std::vector<int>> lists;
    std::vector<int> seen, bosses;
    std::vector<std::string> bossNames, otherNames;
    for (int id : monsterIds) {
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        Ref data(api.Invoke(monsterById, root.get(), {&id}));
        if (!data.get()) continue;
        const bool boss = api.Value<bool>(api.Invoke(isBoss, data.get()));
        if (boss) bosses.push_back(id);
        Ref list(api.Field(data.get(), "achievements"));
        auto numbers = Numbers(list.get());
        if (numbers.empty() && !boss) continue;
        Ref name(api.Invoke(monsterName, data.get()));
        (boss ? bossNames : otherNames).push_back(name.get() ? api.String(name.get()) : "Monstre " + std::to_string(id));
        lists.push_back(std::move(numbers));
    }
    size_t own = 0;
    for (const auto& list : lists) own += list.size();
    size_t fromDungeon = 0;
    if (int map = bosses.empty() ? 0 : HereMap()) {
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
    bossNames.insert(bossNames.end(), otherNames.begin(), otherNames.end());
    target.boss = Title(bossNames);
    if (target.ids.empty() || seen.empty()) return std::nullopt;
    if (!quiet) Log("BOSS_ACHIEVEMENTS monster=" + std::to_string(seen.front()) + " bosses=" + std::to_string(bosses.size()) + " own=" + std::to_string(own) + " dungeon=" + std::to_string(fromDungeon) + " total=" + std::to_string(target.ids.size()));
    return target;
}
void Extend(Obj menu, Obj group) {
    row.armed = false;
    if (!Enabled()) return;
    auto target = FromMonsters(GroupMonsters(group));
    if (!target) return;
    const auto finished = ReadFinished();
    const auto sorted = Sort(ReadItems(target->ids, finished));
    const auto text = MenuLabel(target->boss, sorted, finished.known);
    Ref rows(api.Call(menu, "get_menuItems"));
    int index = api.Value<int>(api.Call(rows.get(), "get_Count"));
    Ref label(api.string_new(text.c_str())), help(api.string_new("Les succès de ces monstres : ceux à faire, et ceux que ce personnage a déjà faits.")), empty(api.string_new(""));
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
        wanted = true; newTarget = true; fightWindow = false;
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
Obj Named(Obj parent, const char* name, int depth = 0) {
    if (!parent || depth > 12) return nullptr;
    Ref hierarchy(api.Call(parent, "get_hierarchy"));
    const int count = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    for (int i = 0; i < count && i < 64; ++i) {
        Ref child(api.Call(hierarchy.get(), "get_Item", {&i}));
        if (api.String(api.Call(child.get(), "get_name")) == name) return child.get();
        if (Obj found = Named(child.get(), name, depth + 1)) return found;
    }
    return nullptr;
}
// The card is laid out for the game's wide window: everything around a text may shrink
// here, and the text wraps instead of being cut by the card's edge.
void FitToWidth(Obj label, Obj stop) {
    int percent = 1; float full = 100;
    Ref length(api.New(api.Class(Ui, UiSpace, "Length"), {&full, &percent}));
    Ref box(Style(label, "maxWidth")); api.Call(box.get(), "set_value", {api.object_unbox(length.get())}); WriteStyle(label, "maxWidth", box.get());
    Set<int>(label, "whiteSpace", 0);
    bool multiline = false;
    api.Call(label, "set_isSingleLine", {&multiline});
    Ref current(label);
    for (int depth = 0; depth < 12 && current.get() && current.get() != stop; ++depth) {
        Pixels(current.get(), "minWidth", 0);
        Set<float>(current.get(), "flexShrink", 1.f);
        current.reset(api.Call(current.get(), "get_parent"));
    }
}
// The game's own card of its achievements window; plain labels if it cannot be had.
bool NativeCard(RowView& view) {
    try {
        Ref settings(api.Invoke(settingsOf, nullptr));
        Ref asset(settings.get() ? api.Invoke(cardAsset, settings.get()) : nullptr);
        if (!asset.get()) return false;
        api.Invoke(cloneTree, asset.get(), {view.line.get()});
        view.card.reset(Named(view.line.get(), "ctr_item"));
        view.name.reset(Named(view.line.get(), "lbl_name"));
        view.points.reset(Named(view.line.get(), "lbl_pointsAchievement"));
        view.description.reset(Named(view.line.get(), "lbl_description"));
        view.icon.reset(Named(view.line.get(), "tx_iconAchievement"));
        if (view.card.get() && view.name.get() && view.points.get() && view.description.get()) {
            FitToWidth(view.description.get(), view.line.get());
            FitToWidth(view.name.get(), view.line.get());
            return true;
        }
        Log("BOSS_ACHIEVEMENTS_CARD_ERROR template");
    } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_CARD_ERROR ") + error.what()); }
    return false;
}
RowView& RowAt(std::vector<RowView>& rows, Obj box, size_t index) {
    while (rows.size() <= index) {
        RowView view;
        view.line.reset(Element(box));
        Pixels(view.line.get(), "marginBottom", 6);
        if (!NativeCard(view)) {
            view.card.reset();
            Ref head(Element(view.line.get()));
            Set<int>(head.get(), "flexDirection", 2);
            view.name.reset(Label(head.get(), "", nativeStyle::Strong));
            Set<float>(view.name.get(), "flexGrow", 1.f); Set<float>(view.name.get(), "flexShrink", 1.f);
            view.points.reset(Label(head.get(), "", nativeStyle::Secondary));
            Pixels(view.points.get(), "width", PointsWidth); Set<float>(view.points.get(), "flexShrink", 0.f);
            Set<int>(view.points.get(), "unityTextAlign", AlignRight);
            view.description.reset(Label(view.line.get(), "", nativeStyle::Secondary));
        }
        rows.push_back(std::move(view));
    }
    return rows[index];
}
void Fill(std::vector<RowView>& rows, Obj box, const std::vector<Item>& items, bool done) {
    for (size_t i = 0; i < items.size(); ++i) {
        auto& view = RowAt(rows, box, i);
        Show(view.line.get(), true);
        Text(view.name.get(), items[i].name);
        Text(view.description.get(), items[i].description);
        if (view.card.get()) {
            Text(view.points.get(), std::to_string(items[i].points));
            Text(view.description.get(), items[i].description, "set_tooltip");
            if (!doneClass.empty()) nativeStyle::Toggle(view.card.get(), doneClass.c_str(), done);
            if (view.icon.get() && iconTypeField && items[i].icon > 0 && view.shownIcon != items[i].icon) {
                view.shownIcon = items[i].icon;
                Ref address(resourceMap::IconAddress(iconTypeField, items[i].icon));
                bool yes = true;
                api.Invoke(loadIcon, nullptr, {view.icon.get(), api.object_unbox(address.get()), nullptr, nullptr, nullptr, &yes, &yes});
            }
        } else {
            nativeStyle::Tone(view.name.get(), done, false);
            Text(view.points.get(), std::to_string(items[i].points) + " pts");
            Show(view.description.get(), !items[i].description.empty());
        }
    }
    for (size_t i = items.size(); i < rows.size(); ++i) { rows[i].achievement = 0; Show(rows[i].line.get(), false); }
    // Which monsters: each objective, ticked once the game has told.
    for (size_t i = 0; i < items.size(); ++i) {
        auto& view = rows[i];
        view.achievement = items[i].id;
        if (!view.clickable) {
            view.clickable = true;
            Ref clickable(api.object_new(clickableClass));
            Obj none = nullptr;
            api.Invoke(clickableCtor, clickable.get(), {none});
            api.Invoke(addManipulator, nullptr, {view.line.get(), clickable.get()});
        }
        if (!done) Ask(items[i].id);
        const auto answer = answers.find(items[i].id);
        const bool known = done || answer != answers.end();
        if (!view.objectivesBox.get()) {
            view.objectivesBox.reset(Element(view.line.get()));
            Pixels(view.objectivesBox.get(), "marginTop", 4); Pixels(view.objectivesBox.get(), "marginLeft", 8);
        }
        const auto& objectives = ObjectivesOf(items[i].id);
        for (size_t k = 0; k < objectives.size() && k < 24; ++k) {
            if (view.objectives.size() <= k) view.objectives.emplace_back(Label(view.objectivesBox.get(), "", nativeStyle::Secondary));
            const bool finished = done || (answer != answers.end() && answer->second.finished.count(objectives[k].id));
            Show(view.objectives[k].get(), true);
            Text(view.objectives[k].get(), ObjectiveLine(objectives[k].name, known, finished));
            nativeStyle::Toggle(view.objectives[k].get(), nativeStyle::ColorPositive, known && finished);
        }
        for (size_t k = objectives.size(); k < view.objectives.size(); ++k) Show(view.objectives[k].get(), false);
    }
}
void Refresh() {
    const auto finished = ReadFinished();
    auto sorted = Sort(ReadItems(shown.ids, finished));
    const auto doneCount = sorted.done.size();
    if (!state.doneShown) sorted.done.clear();
    Text(state.window.get(), "Succès : " + shown.boss, "set_headerTitle");
    Text(state.todoHead.get(), "À faire (" + std::to_string(sorted.todo.size()) + ")");
    Text(state.doneHead.get(), "Déjà faits (" + std::to_string(doneCount) + ")");
    Text(state.showDone.get(), "Afficher ceux déjà faits (" + std::to_string(doneCount) + ")", "set_rightText");
    Show(state.todoHead.get(), !sorted.todo.empty());
    Show(state.doneHead.get(), !sorted.done.empty());
    Fill(state.todo, state.todoBox.get(), sorted.todo, false);
    Fill(state.done, state.doneBox.get(), sorted.done, true);
    Show(state.status.get(), !finished.known);
}
// The checkbox says what the player chose last, but opened from a fight, only what is left.
void SyncShowDone() {
    bool value = state.doneShown = !fightWindow && module->settings.bossAchievementsShowDone;
    api.Call(state.showDone.get(), "set_value", {&value});
}
// Through the game's own link: it opens its achievements window on that achievement.
void OpenInGameWindow(int id) {
    Ref bus(module->FindService("fmz")), tooltips(module->FindService("fou")), theme(module->FindService("fri"));
    if (!bus.get() || !tooltips.get() || !theme.get()) throw std::runtime_error("Achievement link services unavailable");
    int count = 1, index = 0;
    Ref parameters(api.Invoke(createArray, nullptr, {stringType.get(), &count}));
    Ref value(api.string_new(std::to_string(id).c_str())), title(api.string_new(""));
    api.Invoke(setArray, parameters.get(), {value.get(), &index});
    NullableColor color;
    Vec2 position;
    Ref link(api.object_new(linkClass));
    api.Invoke(linkCtor, link.get(), {title.get(), parameters.get(), bus.get(), tooltips.get(), theme.get(), &color, &color});
    api.Invoke(linkOpen, link.get(), {&position});
    Log("BOSS_ACHIEVEMENTS_OPEN_GAME id=" + std::to_string(id));
}
bool Action(Obj owner) {
    if (!owner) return false;
    if (owner == state.closeButton.get() && state.window.get()) { wanted = false; return true; }
    for (auto* rows : {&state.todo, &state.done})
        for (const auto& row : *rows) {
            if (!row.achievement || row.line.get() != owner) continue;
            try { OpenInGameWindow(row.achievement); }
            catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_OPEN_GAME_ERROR ") + error.what()); }
            return true;
        }
    for (const auto& icon : panel.icons) {
        if (icon.get() != owner) continue;
        shown = panel.target;
        wanted = true; newTarget = true; fightWindow = true;
        return true;
    }
    return false;
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
    Pixels(state.window.get(), "width", 460); Pixels(state.window.get(), "height", 420);
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
    state.showDone.reset(api.New(api.Class("Core.dll", "Core.UILogic.Components.Figma", "CheckboxCustom")));
    Pixels(state.showDone.get(), "marginBottom", 8);
    Add(content.get(), state.showDone.get());
    state.status.reset(Label(content.get(), "Le jeu n’a pas encore donné les succès terminés : ouvre une fois la fenêtre des succès.", nativeStyle::Secondary));
    Pixels(state.status.get(), "marginBottom", 8);
    state.todoHead.reset(Label(content.get(), "", nativeStyle::Heading));
    Pixels(state.todoHead.get(), "marginBottom", 6);
    state.todoBox.reset(Element(content.get()));
    state.doneHead.reset(Label(content.get(), "", nativeStyle::Heading));
    Pixels(state.doneHead.get(), "marginTop", 4); Pixels(state.doneHead.get(), "marginBottom", 6);
    state.doneBox.reset(Element(content.get()));
    SyncShowDone();
    Refresh();
    Add(root, state.window.get());
    Log("BOSS_ACHIEVEMENTS_OPEN items=" + std::to_string(shown.ids.size()));
}
void ClearPanelIcons() {
    for (const auto& icon : panel.icons) {
        try { if (icon.get()) api.Call(icon.get(), "RemoveFromHierarchy"); }
        catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_PANEL_ERROR ") + error.what()); }
    }
    panel.icons.clear();
}
void AddPanelIcon(Obj list, const Item& item) {
    Ref icon(api.New(iconClass));
    if (iconTypeField && item.icon > 0) {
        Ref address(resourceMap::IconAddress(iconTypeField, item.icon));
        api.Invoke(updateIcon, icon.get(), {api.object_unbox(address.get())});
    }
    Ref service(module->FindService("fou"));
    if (service.get()) {
        std::vector<std::string> lines;
        const auto answer = answers.find(item.id);
        for (const auto& objective : ObjectivesOf(item.id))
            lines.push_back(ObjectiveLine(objective.name, answer != answers.end(), answer != answers.end() && answer->second.finished.count(objective.id)));
        Ref tip(api.string_new(Tip(item, lines).c_str()));
        Ref manipulator(api.object_new(tooltipClass));
        int origin = TipTooltipLeft, source = TipSourceRight, behavior = TipLeave;
        bool front = false;
        api.Invoke(tooltipCtor, manipulator.get(), {service.get(), tip.get(), &origin, &source, &behavior, &front});
        api.Invoke(addManipulator, nullptr, {icon.get(), manipulator.get()});
    }
    if (!api.Call(icon.get(), "get_clickable")) {
        Ref clickable(api.object_new(clickableClass));
        Obj none = nullptr;
        api.Invoke(clickableCtor, clickable.get(), {none});
        api.Invoke(addManipulator, nullptr, {icon.get(), clickable.get()});
    }
    api.Call(list, "Add", {icon.get()});
    panel.icons.emplace_back(icon.get());
}
void DisplayHook(Obj self, Method method) {
    originalDisplay(self, method);
    try {
        panel.display.reset(self); panel.dirty = true;
        Ref list(*reinterpret_cast<Obj*>(reinterpret_cast<char*>(self) + SuccessListOffset));
        Ref display(list.get() ? Style(list.get(), "display") : nullptr);
        panel.nativeHidden = display.get() && api.Value<int>(api.Call(display.get(), "get_value")) == 1;
    } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_PANEL_ERROR ") + error.what()); }
}
// The panel's list of achievements, for what the enemies still have to give. The game
// empties it whenever it redraws the panel: the next look draws them again.
void WatchFight() {
    if (!module->InCombat()) {
        // What this fight advanced is known again from the server when next needed.
        if (wasInFight) { answers.clear(); asked.clear(); }
        wasInFight = false;
        ClearPanelIcons();
        if (fightWindow && wanted) wanted = false;
        fightWindow = false; panel.display.reset(); panel.enemies.clear(); panel.dirty = false;
        return;
    }
    wasInFight = true;
    if (!module->settings.combatAchievements) { ClearPanelIcons(); panel.enemies.clear(); return; }
    const auto now = GetTickCount64();
    if (now < nextFightRead || !panel.display.get()) return;
    nextFightRead = now + FightRetryMs;
    const auto enemies = soul::ReadEnemyMonsters(module->fight.get());
    if (enemies.ids.empty() || (!panel.dirty && enemies.ids == panel.enemies)) return;
    Ref list(*reinterpret_cast<Obj*>(reinterpret_cast<char*>(panel.display.get()) + SuccessListOffset));
    if (!list.get() || !api.Call(list.get(), "get_panel") || api.String(api.Call(list.get(), "get_name")) != "success_list") return;
    const auto finished = ReadFinished();
    if (!finished.known) return; // Tried again at the next look.
    panel.dirty = false; panel.enemies = enemies.ids;
    ClearPanelIcons();
    if (panel.nativeHidden) {
        // An earlier fight's achievements: they stay out of sight until the game empties them.
        Ref hierarchy(api.Call(list.get(), "get_hierarchy"));
        const int children = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
        for (int i = 0; i < children && i < 64; ++i) { Ref child(api.Call(hierarchy.get(), "get_Item", {&i})); Show(child.get(), false); }
    }
    auto target = FromMonsters(enemies.ids);
    if (!target) return;
    const auto sorted = Sort(ReadItems(target->ids, finished));
    // What the panel shows already, from the fight's challenges.
    Ref native(panel.nativeHidden ? nullptr : api.Field(panel.display.get(), "m_successList"));
    for (const auto& item : sorted.todo) {
        if (panel.icons.size() >= MaxPanelIcons) break;
        int id = item.id;
        if (native.get() && api.Value<bool>(api.Call(native.get(), "ContainsKey", {&id}))) continue;
        // Shown once the game has told it: only if one of these monsters is still missing.
        const auto answer = answers.find(id);
        if (!answersUnavailable) {
            Ask(id);
            if (answer == answers.end() || !Advances(ObjectivesOf(id), answer->second.finished, enemies.ids)) continue;
        }
        AddPanelIcon(list.get(), item);
    }
    panel.target = std::move(*target);
    if (!panel.icons.empty()) Show(list.get(), true);
    Log("BOSS_ACHIEVEMENTS_PANEL icons=" + std::to_string(panel.icons.size()) + " todo=" + std::to_string(sorted.todo.size()));
}
// The game's tooltip of a group, on the map. Its own lines are found again by the
// monsters' names, as nothing else ties them to a monster.
const ActiveObjective* ReadActiveObjective(int id) {
    const auto found = activeObjectives.find(id);
    if (found != activeObjectives.end()) return &found->second;
    ActiveObjective objective;
    Ref root(api.Invoke(questObjectivesRoot, nullptr));
    Ref data(root.get() ? api.Invoke(questObjectiveById, root.get(), {&id}) : nullptr);
    if (!data.get()) return nullptr;
    Ref parameters(api.Field(data.get(), "parameters"));
    if (parameters.get())
        for (const char* name : {"parameter0", "parameter1", "parameter2", "parameter3", "parameter4"})
            objective.parameters.push_back(api.Value<int>(api.Field(parameters.get(), name)));
    Ref words(api.Invoke(objectiveText, data.get()));
    objective.text = words.get() ? Plain(api.String(words.get())) : "";
    objective.folded = itemSearch::Fold(objective.text);
    Ref step(api.Invoke(objectiveStep, data.get()));
    if (step.get()) {
        int quest = api.Value<int>(api.Field(step.get(), "questId"));
        Ref quests(api.Invoke(questsRoot, nullptr));
        Ref questData(quests.get() ? api.Invoke(questById, quests.get(), {&quest}) : nullptr);
        Ref name(questData.get() ? api.Invoke(questName, questData.get()) : nullptr);
        objective.quest = name.get() ? api.String(name.get()) : "";
    }
    return &activeObjectives.emplace(id, std::move(objective)).first->second;
}
Obj StatusIcon(int figma, float r, float g, float b) {
    Ref icon(api.New(statusIconClass));
    api.Invoke(iconSet, icon.get(), {&figma});
    float color[4] = {r, g, b, 1.f};
    api.Invoke(iconColor, icon.get(), {color});
    Pixels(icon.get(), "width", 14); Pixels(icon.get(), "height", 14); Pixels(icon.get(), "marginRight", 4);
    Set<float>(icon.get(), "flexShrink", 0.f);
    return icon.get();
}
void DecorateTooltip(Obj builder) {
    Ref binding(api.Field(builder, "m_binding"));
    Ref group(api.Field(builder, "m_monstersInfo"));
    if (!binding.get() || !group.get()) return;
    Ref content(api.Call(binding.get(), "get_ctr_content")), list(api.Call(binding.get(), "get_ctr_monsterList"));
    if (!content.get() || !list.get()) return;
    if (Ref old(Named(content.get(), TooltipBlockName)); old.get()) api.Call(old.get(), "RemoveFromHierarchy");
    const auto monsters = GroupMonsters(group.get());
    Ref monstersData(api.Invoke(monstersRoot, nullptr));
    if (!monstersData.get()) return;
    std::map<int, std::string> monsterNames;
    for (int id : monsters) {
        Ref data(api.Invoke(monsterById, monstersData.get(), {&id}));
        Ref name(data.get() ? api.Invoke(monsterName, data.get()) : nullptr);
        if (name.get()) monsterNames[id] = api.String(name.get());
    }
    std::set<int> questMonsters, missing, done;
    Ref block(api.New(api.Class(Ui, UiSpace, "VisualElement")));
    Text(block.get(), TooltipBlockName, "set_name");
    bool any = false;
    // The active quests: known to the game without asking.
    Ref service(module->FindService("fnp"));
    if (service.get() && api.object_get_class(service.get()) == questServiceClass) {
        Ref active(api.Field(service.get(), "<dtdg>k__BackingField"));
        size_t shown = 0;
        for (const int id : Numbers(active.get())) {
            const auto* objective = ReadActiveObjective(id);
            if (!objective) continue;
            bool concerned = false;
            for (const auto& [monster, name] : monsterNames)
                if (ObjectiveOnMonster(objective->parameters, monster, objective->folded, itemSearch::Fold(name))) { questMonsters.insert(monster); concerned = true; }
            if (!concerned || shown >= 3) continue;
            ++shown; any = true;
            Ref line(Element(block.get()));
            Set<int>(line.get(), "flexDirection", 2); Pixels(line.get(), "marginBottom", 4);
            Add(line.get(), Ref(StatusIcon(IconExclamation, 0.85f, 0.95f, 0.2f)).get());
            Ref words(Element(line.get()));
            Set<float>(words.get(), "flexShrink", 1.f); Set<float>(words.get(), "flexGrow", 1.f);
            Ref title(Label(words.get(), objective->quest, nativeStyle::Strong));
            Ref detail(Label(words.get(), objective->text, nativeStyle::Secondary));
            (void)title; (void)detail;
        }
    }
    // The achievements still missing one of these monsters.
    if (auto target = FromMonsters(monsters, true)) {
        const auto finished = ReadFinished();
        if (finished.known) {
            const auto sorted = Sort(ReadItems(target->ids, finished));
            const Item* best = nullptr;
            Progress bestProgress;
            int others = 0;
            for (const auto& item : sorted.todo) {
                const auto& objectives = ObjectivesOf(item.id);
                if (answersUnavailable) continue;
                Ask(item.id);
                const auto answer = answers.find(item.id);
                if (answer == answers.end()) continue;
                for (int monster : monsters) {
                    const auto mark = MarkOf(monster, objectives, answer->second.finished);
                    if (mark == Mark::Missing) missing.insert(monster);
                    else if (mark == Mark::Done) done.insert(monster);
                }
                if (!Advances(objectives, answer->second.finished, monsters)) continue;
                const auto progress = Measure(objectives, answer->second.finished);
                if (!best || Percent(progress) > Percent(bestProgress)) { if (best) ++others; best = &item; bestProgress = progress; }
                else ++others;
            }
            if (best) {
                any = true;
                RowView view;
                view.line.reset(Element(block.get()));
                if (NativeCard(view)) {
                    Text(view.name.get(), best->name);
                    Text(view.points.get(), std::to_string(best->points));
                    std::string line = bestProgress.total ? Summary(bestProgress) : best->description;
                    if (others) line += "\n+ " + std::to_string(others) + (others == 1 ? " autre succès" : " autres succès");
                    Text(view.description.get(), line);
                    if (view.icon.get() && iconTypeField && best->icon > 0) {
                        Ref address(resourceMap::IconAddress(iconTypeField, best->icon));
                        bool yes = true;
                        api.Invoke(loadIcon, nullptr, {view.icon.get(), api.object_unbox(address.get()), nullptr, nullptr, nullptr, &yes, &yes});
                    }
                }
            }
        }
    }
    if (any) {
        Pixels(block.get(), "marginTop", 4); Pixels(block.get(), "marginBottom", 4);
        for (const char* side : {"paddingLeft", "paddingRight"}) Pixels(block.get(), side, 8);
        int index = api.Value<int>(api.Call(content.get(), "IndexOf", {list.get()}));
        if (index < 0) index = 0;
        api.Call(content.get(), "Insert", {&index, block.get()});
    }
    // The group's lines.
    Ref hierarchy(api.Call(list.get(), "get_hierarchy"));
    const int lines = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    for (int i = 0; i < lines && i < 32; ++i) {
        Ref line(api.Call(hierarchy.get(), "get_Item", {&i}));
        Ref label(Named(line.get(), "lbl_monsterName"));
        if (!label.get()) continue;
        const auto shownText = api.String(api.Call(label.get(), "get_text"));
        int monster = 0; size_t longest = 0;
        for (const auto& [id, name] : monsterNames)
            if (name.size() > longest && shownText.rfind(name, 0) == 0) { monster = id; longest = name.size(); }
        if (!monster) continue;
        Ref parent(api.Call(label.get(), "get_parent"));
        if (!parent.get()) continue;
        Set<int>(parent.get(), "flexDirection", 2);
        const bool quest = questMonsters.count(monster) != 0, lacking = missing.count(monster) != 0, finished = !lacking && done.count(monster) != 0;
        int at = api.Value<int>(api.Call(parent.get(), "IndexOf", {label.get()}));
        if (at < 0) at = 0;
        const auto put = [&](int figma, float r, float g, float b) {
            Ref icon(StatusIcon(figma, r, g, b));
            api.Call(parent.get(), "Insert", {&at, icon.get()});
            ++at;
        };
        if (quest) put(IconExclamation, 0.85f, 0.95f, 0.2f);
        if (lacking) put(IconTrophy, 0.7f, 0.7f, 0.7f);
        if (finished) put(IconTick, 0.45f, 0.9f, 0.35f);
        if (quest) nativeStyle::Add(label.get(), "textColor_highlight_primary100");
        else if (finished) nativeStyle::Add(label.get(), nativeStyle::ColorPositive);
    }
}
void TooltipSetupHook(Obj builder, Method method) {
    originalTooltipSetup(builder, method);
    if (!TooltipEnabled() || module->InCombat()) return;
    try { tooltipView.builder.reset(builder); tooltipView.dirty = false; DecorateTooltip(builder); }
    catch (const std::exception& error) {
        if (!tooltipReported) Log(std::string("MONSTER_TOOLTIP_ERROR ") + error.what());
        tooltipReported = true;
    }
}
// The server's answer has come while the tooltip is shown.
void RefreshTooltip() {
    if (!tooltipView.dirty || !tooltipView.builder.get()) return;
    tooltipView.dirty = false;
    Ref binding(api.Field(tooltipView.builder.get(), "m_binding"));
    Ref content(binding.get() ? api.Call(binding.get(), "get_ctr_content") : nullptr);
    if (!content.get() || !api.Call(content.get(), "get_panel")) { tooltipView.builder.reset(); return; }
    Ref list(api.Call(binding.get(), "get_ctr_monsterList"));
    // The marks of the lines are drawn again with them.
    Ref hierarchy(api.Call(list.get(), "get_hierarchy"));
    const int lines = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
    for (int i = 0; i < lines && i < 32; ++i) {
        Ref line(api.Call(hierarchy.get(), "get_Item", {&i}));
        Ref label(Named(line.get(), "lbl_monsterName"));
        Ref parent(label.get() ? api.Call(label.get(), "get_parent") : nullptr);
        if (!parent.get()) continue;
        Ref children(api.Call(parent.get(), "get_hierarchy"));
        for (int k = api.Value<int>(api.Call(children.get(), "get_childCount")); k-- > 0;) {
            Ref child(api.Call(children.get(), "get_Item", {&k}));
            if (child.get() && api.object_get_class(child.get()) == statusIconClass && api.String(api.Call(child.get(), "get_name")).empty() && child.get() != label.get()) {
                const int figma = api.Value<int>(api.Call(child.get(), "get_icon"));
                if (figma == IconExclamation || figma == IconTrophy || figma == IconTick) api.Call(child.get(), "RemoveFromHierarchy");
            }
        }
    }
    DecorateTooltip(tooltipView.builder.get());
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
        Field(achievement, "points", 36); Field(achievement, "level", 40); Field(achievement, "categoryId", 24); Field(achievement, "iconId", 32);
        const auto category = api.Class("Core.dll", "", "nz");
        categoryOf = api.FindVerified(category, "ova", 1, 0x2D09FB0);
        categoryVisible = api.FindVerified(category, "ovb", 1, 0x2D0A0D0);
        const auto settings = api.Class("Core.dll", "Core.UILogic.Components", "DofusUISettings");
        settingsOf = api.FindVerified(settings, "GetSettings", 0, 0x1591A70);
        cardAsset = api.FindVerified(settings, "get_achievementItem", 0, 0x1592E50);
        cloneTree = api.FindVerified(api.Class(Ui, UiSpace, "VisualTreeAsset"), "CloneTree", 1, 0x5643780);
        loadIcon = api.FindVerified(api.Class("Core.dll", "", "pq"), "paw", 7, 0x2E547E0);
        resourceMap::InstallIcons();
        iconTypeField = api.class_get_field_from_name(api.Class("Core.dll", "", "rz"), "deuy");
        if (!iconTypeField || api.field_get_offset(iconTypeField) != 800) throw std::runtime_error("Achievement icon catalog layout");
        const auto display = api.Class("Core.dll", "Core.UILogic.Fight", "UIChallengeSelectionDisplay");
        Field(display, "m_successList", 312);
        Field(api.Class("Core.dll", "Core.UILogic.Bindings", "ChallengeSelectionDisplayBinding"), "_success_list", 72);
        iconClass = api.Class("Core.dll", "Core.UILogic.Components.Figma", "ChallengeIcon");
        api.FindVerified(iconClass, ".ctor", 0, 0x16FCD40);
        api.FindVerified(iconClass, "get_clickable", 0, 0x158B930);
        updateIcon = api.FindVerified(iconClass, "UpdateChallengeIcon", 1, 0x16FC960);
        addManipulator = api.FindVerified(api.Class(Ui, UiSpace, "VisualElementExtensions"), "AddManipulator", 2, 0x563A930);
        clickableClass = api.Class("Core.dll", "Core.UILogic.Components.Figma", "ExtendedClickable");
        clickableCtor = api.FindVerified(clickableClass, ".ctor", 1, 0x1725300);
        tooltipClass = api.Class("Core.dll", "Core.UILogic.Components.Tooltips", "TooltipManipulator");
        tooltipCtor = api.FindVerified(tooltipClass, ".ctor", 6, 0x15D9580);
        Field(achievement, "objectiveIds", 56);
        objectivesRoot = api.FindVerified(data, "get_achievementObjectivesDataRoot", 0, 0x7B1130);
        const auto objective = api.Class(DataImage, QuestSpace, "AchievementObjectiveData");
        Field(objective, "criterion", 32);
        objectiveName = api.FindVerified(objective, "get_name", 0, 0x7AEB40);
        const auto checkbox = api.Class("Core.dll", "Core.UILogic.Components.Figma", "CheckboxCustom");
        api.FindVerified(checkbox, ".ctor", 0, 0x16FE760); api.FindVerified(checkbox, "get_value", 0, 0x16FEAE0);
        api.FindVerified(checkbox, "set_value", 1, 0x16FF100); api.FindVerified(checkbox, "set_rightText", 1, 0x16FF020);
        // As AchievementsUI.OnAchievementSelected: cvo, handled by fcr, which sends mdk. Many
        // one-short messages share this constructor's address: the class is named here by
        // what the window allocates, and checked on the handler's own parameter.
        senderClass = api.Class("Core.dll", "", "fcr");
        api.FindVerified(senderClass, ".ctor", 3, 0xDDD4F0);
        sendDetails = api.FindVerified(senderClass, "bcus", 1, 0x1AA4B20);
        requestClass = api.Class("Core.dll", "", "cvo");
        api.FindVerified(requestClass, ".ctor", 1, 0x26384F0);
        if (api.class_from_type(api.method_get_param(sendDetails, 0)) != requestClass) throw std::runtime_error("Achievement details request class");
        const auto links = "Core.UILogic.Components.Hyperlink";
        linkClass = api.Class("Core.dll", links, "HyperlinkShowAchievement");
        linkCtor = api.FindVerified(linkClass, ".ctor", 7, 0x1698450);
        linkOpen = api.FindVerified(linkClass, "OnHyperlinkLeftClick", 1, 0x16983A0);
        const auto array = api.Class("mscorlib.dll", "System", "Array");
        createArray = api.FindVerified(array, "CreateInstance", 2, 0x42C58D0);
        setArray = api.FindVerified(array, "SetValue", 2, 0x42C82C0);
        stringType.reset(api.type_get_object(api.class_get_type(api.Class("mscorlib.dll", "System", "String"))));
        const auto details = api.Class("Core.dll", "", "gw");
        answerOf = api.FindVerified(details, "mxn", 1, 0x25992F0);
        Field(details, "dcmb", 16); Field(details, "dcmc", 24);
        Field(api.Class("Core.dll", "", "gv"), "dcly", 16);
        Field(api.Class("Ankama.Dofus.Protocol.Game.dll", "", "mdv"), "fgtm", 24);
        const auto questSpace = "Core.DataCenter.Metadata.Quest";
        questObjectivesRoot = api.FindVerified(data, "get_questObjectivesDataRoot", 0, 0X7B7030);
        questObjectiveById = api.FindVerified(api.Class(DataImage, "Core.DataCenter.Metadata.Quest", "QuestObjectivesDataRoot"), "GetQuestObjectiveById", 1, 0X7C2220);
        questsRoot = api.FindVerified(data, "get_questsDataRoot", 0, 0X7B7210);
        questById = api.FindVerified(api.Class(DataImage, "Core.DataCenter.Metadata.Quest", "QuestsDataRoot"), "GetQuestById", 1, 0X7C27D0);
        const auto questData = api.Class(DataImage, questSpace, "QuestData");
        questName = api.FindVerified(questData, "get_name", 0, 0x7BFB90);
        const auto questObjective = api.Class(DataImage, questSpace, "QuestObjectiveData");
        Field(questObjective, "parameters", 32);
        objectiveText = api.FindVerified(questObjective, "get_text", 0, 0x7C0E10);
        objectiveStep = api.FindVerified(questObjective, "get_questStep", 0, 0x7C0D80);
        Field(api.Class(DataImage, questSpace, "QuestStepData"), "questId", 20);
        const auto questParameters = api.Class(DataImage, "Core.DataCenter.Metadata.Quest.Objective", "QuestObjectiveParametersData");
        Field(questParameters, "parameter0", 20); Field(questParameters, "parameter4", 36);
        questServiceClass = api.Class("Core.dll", "", "fdi");
        Field(questServiceClass, "<dtdg>k__BackingField", 120); // Its active objectives (fdi.bdah).
        statusIconClass = api.Class("Core.dll", "Core.UILogic.Components.Figma", "DofusIcon");
        iconCtor = api.FindVerified(statusIconClass, ".ctor", 0, 0x1708660);
        iconSet = api.FindVerified(statusIconClass, "set_icon", 1, 0x1708F10);
        iconColor = api.FindVerified(statusIconClass, "set_color", 1, 0x1708CC0);
        api.FindVerified(statusIconClass, "get_icon", 0, 0x1591900);
        const auto visual = api.Class(Ui, UiSpace, "VisualElement");
        api.FindVerified(visual, "Insert", 2, 0x55AAA50); api.FindVerified(visual, "IndexOf", 1, 0x55AA8F0);
        const auto tooltipBinding = api.Class("Core.dll", "Core.UI.Bindings", "WorldRpMonstersGroupTooltipBinding");
        api.FindVerified(tooltipBinding, "get_ctr_content", 0, 0x18A2010); api.FindVerified(tooltipBinding, "get_ctr_monsterList", 0, 0x18A2390);
        const auto groupTooltip = api.Class("Core.dll", "Core.UILogic.Components.Tooltips.Builder", "WorldRpMonstersGroupTooltipBuilder");
        Field(groupTooltip, "m_monstersInfo", 320);
        AddHook(api.FindVerified(groupTooltip, "SetupViewWithContent", 0, 0x1666600), 0x1666600, reinterpret_cast<void*>(&TooltipSetupHook), reinterpret_cast<void**>(&originalTooltipSetup));
        const auto handler = api.Class("Core.dll", "", "fcv");
        AddHook(api.FindVerified(handler, "bcvi", 1, 0x1AA7320), 0x1AA7320, reinterpret_cast<void*>(&AnswerHook<0>), reinterpret_cast<void**>(&originalAnswer[0]));
        AddHook(api.FindVerified(handler, "jnm", 1, 0x1AAEB40), 0x1AAEB40, reinterpret_cast<void*>(&AnswerHook<1>), reinterpret_cast<void**>(&originalAnswer[1]));
        AddHook(api.FindVerified(display, "DisplayChallenges", 0, 0x2CA08D0), 0x2CA08D0, reinterpret_cast<void*>(&DisplayHook), reinterpret_cast<void**>(&originalDisplay));
        // The card's class of a finished achievement (AchievementItem.Init). Without it,
        // the section's title still says so.
        try {
            doneClassOf = api.FindVerified(api.Class("Core.dll", "<PrivateImplementationDetails>{CB22598B-AE9F-4989-A466-01138C0A3A23}", "a"), "nvp", 0, 0x242AA10);
        } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_DONE_CLASS ") + error.what()); }
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
        finishedAccount = api.FindVerified(serviceClass, "bcwx", 0, 0x69A650);
        finishedCharacter = api.FindVerified(serviceClass, "bcxd", 0, 0x73FA90);
        currentMap = api.FindVerified(api.Class("Core.dll", "", "esg"), "zeo", 0, 0x1854C30);
        append = api.FindVerified(api.Class("Core.dll", "Core.UILogic.Components.ContextMenu", "DofusContextualMenu"), "AppendDefaultItem", 13, 0x16D06F0);
        unsubscribe = api.FindVerified(api.Class("Core.dll", "Core.Engine.Inputs.Events", "InputCustomEvent"), "UnSubscribe", 1, 0x2097300);
        soul::SetAuxiliaryPanelMenus(NoPopulate, Clicked);
        soul::SetAuxiliaryPanelAction(Action); nativeClick::AddHandler(Action);
        toolsUi::Register(Key, "Succès du monstre", IconFlag, [] { return wanted; }, [] { wanted = !wanted && !shown.ids.empty(); return true; }, {},
            [] { return false; });
        SetSoulGroupMenuExtension(Extend);
        installed = true;
        Log("BOSS_ACHIEVEMENTS_READY");
        Log("MONSTER_TOOLTIP_READY");
    } catch (const std::exception& error) {
        Log(std::string("BOSS_ACHIEVEMENTS_UNAVAILABLE ") + error.what());
    }
}

void RestoreBossAchievements() {
    wanted = false; newTarget = false; row.armed = false; fightWindow = false;
    if (!Session()) ClearPanelIcons();
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
    if (Enabled()) {
        try {
            if (doneClass.empty() && doneClassOf) { Ref name(api.Invoke(doneClassOf, nullptr)); doneClassOf = nullptr; if (name.get()) doneClass = api.String(name.get()); }
            WatchFight();
        }
        catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_FIGHT_ERROR ") + error.what()); }
    }
    if (Session()) {
        try { PumpQuestions(); RefreshTooltip(); }
        catch (const std::exception& error) {
            if (!tooltipReported) Log(std::string("MONSTER_TOOLTIP_ERROR ") + error.what());
            tooltipReported = true;
        }
    }
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
        if (newTarget) { SyncShowDone(); state.nextRefresh = 0; }
        const bool chosen = api.Value<bool>(api.Call(state.showDone.get(), "get_value"));
        if (chosen != state.doneShown) {
            state.doneShown = chosen; state.nextRefresh = 0;
            if (!fightWindow && module->settings.bossAchievementsShowDone != chosen) {
                Settings next = module->settings;
                next.bossAchievementsShowDone = chosen;
                const bool withMetamobSync = module->KeepSharedMetamobSync(next);
                module->settings = next;
                module->SaveSettings(withMetamobSync);
            }
        }
        if (newTarget || now >= state.nextRefresh) {
            newTarget = false; state.nextRefresh = now + RefreshMs;
            Refresh();
        }
    } catch (const std::exception& error) { Log(std::string("BOSS_ACHIEVEMENTS_ERROR ") + error.what()); RestoreBossAchievements(); }
}
