#include "jobs/recipe-native.hpp"
#include "core/module.hpp"
#include "ui/item-search-policy.hpp"
#include <algorithm>
#include <set>

namespace recipeNative {
namespace {
using namespace managed;
constexpr const char* DataImage = "Ankama.Dofus.Core.DataCenter.dll";
constexpr const char* JobSpace = "Core.DataCenter.Metadata.Job";
Method recipesRoot = nullptr, recipeByResult = nullptr, itemById = nullptr, itemName = nullptr, averagePrice = nullptr;
Method bagInventory = nullptr, quantityOf = nullptr, objectNumber = nullptr, itemWithGid = nullptr, bankOf = nullptr, havenBagOf = nullptr, viewOf = nullptr, viewContent = nullptr;
constexpr int ViewReal = 2, ViewBank = 10; // ViewsNames: the bag's whole content, the bank's.
Klass priceClass = nullptr, inventoryClass = nullptr;
bool installed = false;

void Field(Klass type, const char* name, size_t offset) {
    const auto field = api.class_get_field_from_name(type, name);
    if (!field || api.field_get_offset(field) != offset) throw std::runtime_error(std::string("Recipe field layout: ") + name);
}
bool Session() { return installed && module && module->servicesReady && !module->stopped; }
Obj Service(const char* name, Klass expected) {
    Obj service = module->FindService(name);
    return service && api.object_get_class(service) == expected ? service : nullptr;
}
std::vector<int> Numbers(Obj list) {
    std::vector<int> result;
    const int count = list ? api.Value<int>(api.Call(list, "get_Count")) : 0;
    if (count < 0 || count > 64) throw std::runtime_error("Recipe list bounds");
    for (int i = 0; i < count; ++i) {
        Ref value(api.Call(list, "get_Item", {&i}));
        result.push_back(api.Value<int>(value.get()));
    }
    return result;
}
int64_t Quantity(Obj inventory, int item) {
    Ref value(api.Invoke(quantityOf, inventory, {&item}));
    return api.Value<int>(value.get());
}
}

void Install() {
    if (installed) return;
    const auto data = api.Class(DataImage, "Core.DataCenter", "DataCenterModule");
    recipesRoot = api.FindVerified(data, "get_recipesDataRoot", 0, 0x7B7350);
    recipeByResult = api.FindVerified(api.Class(DataImage, JobSpace, "RecipesDataRoot"), "GetRecipeByResultId", 1, 0x7E08C0);
    const auto recipe = api.Class(DataImage, JobSpace, "RecipeData");
    Field(recipe, "resultId", 16); Field(recipe, "resultLevel", 36); Field(recipe, "ingredientIds", 40); Field(recipe, "quantities", 48);
    const auto item = api.Class(DataImage, "Core.DataCenter.Metadata.Item", "ItemData");
    itemById = api.FindVerified(item, "GetItemById", 1, 0x7DB910);
    itemName = api.FindVerified(item, "get_name", 0, 0x7DBCD0);
    priceClass = api.Class("Core.dll", "", "ezo");
    averagePrice = api.FindVerified(priceClass, "bbuc", 1, 0x19DBDA0);
    const auto inventories = api.Class("Core.dll", "", "feh");
    bagInventory = api.FindVerified(inventories, "bdoq", 0, 0x11E4900);
    Field(inventories, "dtji", 256); // The bank's inventory, null until the game creates it.
    inventoryClass = inventories;
    const auto inventory = api.Class("Core.dll", "Core.UILogic.Inventory", "Inventory");
    quantityOf = api.FindVerified(inventory, "GetItemsQuantityFromGid", 1, 0x2AAEF70);
    objectNumber = api.FindVerified(inventory, "get_objectNumber", 0, 0x2AB2140);
    itemWithGid = api.FindVerified(inventory, "GetItemWithGid", 1, 0x2AAEE50);
    viewOf = api.FindVerified(inventory, "GetView", 1, 0x2AAF170);
    viewContent = api.FindVerified(api.Class("Core.dll", "Core.UILogic.Inventory.Views", "AbstractStorageView"), "get_content", 0, 0x2AAA950);
    const auto places = api.Class("Core.dll", "", "lz");
    bankOf = api.FindVerified(places, "odz", 0, 0x2A64B50);
    havenBagOf = api.FindVerified(places, "oea", 0, 0x2A64B80);
    api.FindVerified(api.Class("Core.dll", "", "mc"), "myq", 0, 0x744C80);
    installed = true;
}

std::optional<Recipe> Find(int resultItem) {
    if (!Session()) return std::nullopt;
    Ref root(api.Invoke(recipesRoot, nullptr));
    Ref recipe(root.get() ? api.Invoke(recipeByResult, root.get(), {&resultItem}) : nullptr);
    if (!recipe.get()) return std::nullopt;
    Recipe result{resultItem, api.Value<int>(api.Field(recipe.get(), "resultLevel")), {}};
    Ref ids(api.Field(recipe.get(), "ingredientIds")), quantities(api.Field(recipe.get(), "quantities"));
    const auto idList = Numbers(ids.get()), quantityList = Numbers(quantities.get());
    if (idList.empty() || idList.size() != quantityList.size()) return std::nullopt;
    for (size_t i = 0; i < idList.size(); ++i) {
        int id = idList[i];
        Ref data(api.Invoke(itemById, nullptr, {&id}));
        Ref name(data.get() ? api.Invoke(itemName, data.get()) : nullptr);
        result.ingredients.push_back({id, quantityList[i], name.get() ? api.String(name.get()) : "Objet " + std::to_string(id)});
    }
    return result;
}

int64_t AveragePrice(int item) {
    if (!Session()) return 0;
    try {
        Obj service = Service("fjt", priceClass);
        if (!service) return 0;
        Ref value(api.Invoke(averagePrice, service, {&item}));
        return api.Value<int64_t>(value.get());
    } catch (const std::exception&) { return 0; }
}

Stock Owned(int item) {
    Stock stock;
    if (!Session()) return stock;
    try {
        Obj service = Service("fmo", inventoryClass);
        if (!service) return stock;
        Ref bag(api.Invoke(bagInventory, service));
        if (bag.get()) { stock.bag = Quantity(bag.get(), item); stock.bagKnown = true; }
        Ref bank(api.Field(service, "dtji"));
        if (bank.get() && api.Value<int>(api.Invoke(objectNumber, bank.get())) > 0) {
            stock.bank = Quantity(bank.get(), item);
            stock.bankKnown = true;
        }
        // An item of the bag knows what the bank and the havenbag hold of it, as its
        // tooltip shows, whether or not the bank has been opened.
        Ref wrapper(bag.get() ? api.Invoke(itemWithGid, bag.get(), {&item}) : nullptr);
        Ref places(wrapper.get() ? api.Call(wrapper.get(), "myq") : nullptr);
        if (places.get()) {
            stock.bank = std::max<int64_t>(stock.bank, api.Value<int>(api.Invoke(bankOf, places.get())));
            stock.bankKnown = true;
            stock.havenBag = api.Value<int>(api.Invoke(havenBagOf, places.get()));
            stock.havenBagKnown = true;
        }
    } catch (const std::exception&) {}
    return stock;
}

std::vector<Found> Search(const std::string& query) {
    std::vector<Found> found;
    if (!Session()) return found;
    Obj service = Service("fmo", inventoryClass);
    if (!service) return found;
    std::set<int> seen;
    const auto scan = [&](Obj inventory, int viewName) {
        if (!inventory) return;
        Ref view(api.Invoke(viewOf, inventory, {&viewName}));
        Ref items(view.get() ? api.Invoke(viewContent, view.get()) : nullptr);
        const int count = items.get() ? api.Value<int>(api.Call(items.get(), "get_Count")) : 0;
        if (count < 0 || count > 20000) throw std::runtime_error("Inventory view bounds");
        for (int i = 0; i < count; ++i) {
            Ref item(api.Call(items.get(), "get_Item", {&i}));
            Ref data(item.get() ? api.Call(item.get(), "myx") : nullptr);
            if (!data.get()) continue;
            const int id = api.Value<int>(api.Field(data.get(), "id"));
            if (!id || !seen.insert(id).second) continue;
            Ref name(api.Invoke(itemName, data.get()));
            const auto text = name.get() ? api.String(name.get()) : std::string();
            if (itemSearch::Matches(text, query)) found.push_back({id, text, Owned(id)});
        }
    };
    Ref bag(api.Invoke(bagInventory, service)), bank(api.Field(service, "dtji"));
    scan(bag.get(), ViewReal);
    scan(bank.get(), ViewBank);
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.name < b.name; });
    return found;
}
}
