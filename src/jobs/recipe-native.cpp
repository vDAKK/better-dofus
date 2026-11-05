#include "jobs/recipe-native.hpp"
#include "core/module.hpp"

namespace recipeNative {
namespace {
using namespace managed;
constexpr const char* DataImage = "Ankama.Dofus.Core.DataCenter.dll";
constexpr const char* JobSpace = "Core.DataCenter.Metadata.Job";
Method recipesRoot = nullptr, recipeByResult = nullptr, itemById = nullptr, itemName = nullptr, averagePrice = nullptr;
Method bagInventory = nullptr, quantityOf = nullptr, objectNumber = nullptr;
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
    } catch (const std::exception&) {}
    return stock;
}
}
