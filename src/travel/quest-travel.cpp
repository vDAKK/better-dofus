#include "travel/quest-travel.hpp"
#include "travel/chat-tp-links.hpp"
#include "travel/chat-travel.hpp"
#include "core/module.hpp"
#include "souls/soul-panel.hpp"
#include <optional>

namespace {
using namespace managed;
constexpr const char* DataImage = "Ankama.Dofus.Core.DataCenter.dll";
constexpr const char* WorldSpace = "Core.DataCenter.Metadata.World";
constexpr uintptr_t BuilderRva = 0x29E99E0;
constexpr int IconZaap = 336;
using BuilderFn = void (*)(Obj, Obj, Obj, Method);
BuilderFn originalBuilder = nullptr;
Method mapInfo = nullptr, subArea = nullptr, worldOf = nullptr, append = nullptr, currentMap = nullptr;
struct Row { Ref item, callback; ChatTpTarget target{}; bool armed = false; } row;

void Field(Klass type, const char* name, size_t offset) {
    const auto field = api.class_get_field_from_name(type, name);
    if (!field || api.field_get_offset(field) != offset) throw std::runtime_error(std::string("Quest travel field layout: ") + name);
}
bool Enabled() { return module && module->ready && module->servicesReady && !module->stopped && !module->stopRequested && module->settings.questTravel; }
// x, y and world of a map, from the game's data.
std::optional<ChatTpTarget> OfMap(int64_t map) {
    Ref info(api.Invoke(mapInfo, nullptr, {&map}));
    if (!info.get()) return std::nullopt;
    Ref area(api.Invoke(subArea, info.get()));
    if (!area.get()) return std::nullopt;
    Ref world(api.Invoke(worldOf, area.get()));
    return ChatTpTarget{api.Value<short>(api.Field(info.get(), "posX")), api.Value<short>(api.Field(info.get(), "posY")), api.Value<int>(world.get())};
}
// An objective's position: its map, else its coordinates in the character's world.
std::optional<ChatTpTarget> Target(Obj objective) {
    const int64_t map = api.Value<int>(api.Field(objective, "mapId"));
    if (map > 0) if (auto target = OfMap(map)) return target;
    Ref coords(api.Field(objective, "coords"));
    if (!coords.get()) return std::nullopt;
    // The class of this field is not found by its name: its layout is checked on the value.
    const auto coordsClass = api.object_get_class(coords.get());
    Field(coordsClass, "x", 16); Field(coordsClass, "y", 20);
    if (!api.Value<bool>(api.Call(coords.get(), "get_isValid"))) return std::nullopt;
    Ref character(module->FindService("fng"));
    if (!character.get()) return std::nullopt;
    if (api.object_get_class(character.get()) != api.Class("Core.dll", "", "esg")) return std::nullopt;
    Ref location(api.Invoke(currentMap, character.get()));
    if (!location.get() || api.object_get_class(location.get()) != api.Class("Core.dll", "", "tk")) return std::nullopt;
    const int64_t here = *static_cast<int64_t*>(api.FieldAddress(location.get(), "m_mapId"));
    const auto current = here > 0 ? OfMap(here) : std::nullopt;
    if (!current) return std::nullopt;
    return ChatTpTarget{api.Value<int>(api.Field(coords.get(), "x")), api.Value<int>(api.Field(coords.get(), "y")), current->world};
}
// Without a position the row stays, disabled: it says why there is no trip.
void AppendRow(Obj menu, const std::optional<ChatTpTarget>& target) {
    const std::string text = target ? "Voyager jusqu’à l’objectif [" + std::to_string(target->x) + "," + std::to_string(target->y) + "]" : "Voyager : position non indiquée par le jeu";
    Ref rows(api.Call(menu, "get_menuItems"));
    const int index = api.Value<int>(api.Call(rows.get(), "get_Count"));
    Ref label(api.string_new(text.c_str())), help(api.string_new("Lance le trajet du mod vers la position de cet objectif.")), empty(api.string_new(""));
    Ref asset(api.object_new(api.class_from_type(api.method_get_param(append, 7))));
    Ref color(api.object_new(api.class_from_type(api.method_get_param(append, 10))));
    bool disabled = !target, close = true; unsigned delay = 1000; int style = 1, icon = IconZaap;
    api.Invoke(append, menu, {label.get(), nullptr, &disabled, &close, help.get(), &delay, empty.get(),
        api.object_unbox(asset.get()), &style, &icon, api.object_unbox(color.get()), empty.get(), empty.get()});
    row.item.reset(api.Call(rows.get(), "get_Item", {const_cast<int*>(&index)}));
    row.callback.reset(api.Call(row.item.get(), "get_m_callback"));
    if (target) row.target = *target;
    row.armed = target && row.callback.get();
}
void BuilderHook(Obj self, Obj objective, Obj event, Method method) {
    originalBuilder(self, objective, event, method);
    row.armed = false;
    if (!Enabled() || !objective || !event) return;
    try {
        const auto target = Target(objective);
        Ref menu(api.Call(event, "get_menu"));
        if (menu.get()) AppendRow(menu.get(), target);
        // What the game's data gives each objective: many get their flag from the server.
        Ref coords(api.Field(objective, "coords"));
        Log("QUEST_TRAVEL_OBJECTIVE id=" + std::to_string(api.Value<int>(api.Field(objective, "id"))) + " type=" + std::to_string(api.Value<int>(api.Field(objective, "typeId"))) +
            " map=" + std::to_string(api.Value<int>(api.Field(objective, "mapId"))) + " coords=" + (coords.get() ? "1" : "0") + " target=" + (target ? "1" : "0"));
    } catch (const std::exception& error) { Log(std::string("QUEST_TRAVEL_MENU_ERROR ") + error.what()); }
}
bool NoPopulate(Obj, Obj) { return false; }
bool Clicked(Obj callback, Obj view) {
    if (!callback || callback != row.callback.get()) return false;
    try {
        if (!row.armed || !view || api.Call(view, "get_itemData") != row.item.get() || !Enabled()) return true;
        row.armed = false; // Before the trip, which may close the menu.
        Log("QUEST_TRAVEL x=" + std::to_string(row.target.x) + " y=" + std::to_string(row.target.y) + " world=" + std::to_string(row.target.world));
        StartChatTravel(row.target);
    } catch (const std::exception& error) { Log(std::string("QUEST_TRAVEL_ERROR ") + error.what()); }
    return true;
}
}

void InstallQuestTravel() {
    try {
        const auto objective = api.Class(DataImage, "Core.DataCenter.Metadata.Quest", "QuestObjectiveData");
        Field(objective, "id", 16); Field(objective, "typeId", 24); Field(objective, "coords", 40); Field(objective, "mapId", 48);
        const auto info = api.Class(DataImage, WorldSpace, "MapInformationData");
        Field(info, "posX", 24); Field(info, "posY", 26);
        mapInfo = api.FindVerified(info, "GetMapInformationById", 1, 0x7BB050);
        subArea = api.FindVerified(info, "get_subArea", 0, 0x7BB300);
        worldOf = api.FindVerified(api.Class(DataImage, WorldSpace, "SubAreaData"), "get_worldmapId", 0, 0x7C7E90);
        currentMap = api.FindVerified(api.Class("Core.dll", "", "esg"), "zeo", 0, 0x1854C30);
        append = api.FindVerified(api.Class("Core.dll", "Core.UILogic.Components.ContextMenu", "DofusContextualMenu"), "AppendDefaultItem", 13, 0x16D06F0);
        soul::SetAuxiliaryPanelMenus(NoPopulate, Clicked);
        AddHook(api.FindVerified(api.Class("Core.dll", "Core.UILogic.Quest", "QuestList"), "ObjectiveContextMenuBuilder", 2, BuilderRva), BuilderRva,
            reinterpret_cast<void*>(&BuilderHook), reinterpret_cast<void**>(&originalBuilder));
        Log("QUEST_TRAVEL_READY");
    } catch (const std::exception& error) {
        Log(std::string("QUEST_TRAVEL_UNAVAILABLE ") + error.what());
    }
}
