#include "jobs/jobs-xp-panel.hpp"
#include "jobs/jobs-xp-data.hpp"
#include "jobs/recipe-cost.hpp"
#include "jobs/recipe-native.hpp"
#include "core/module.hpp"
#include "souls/soul-panel.hpp"
#include "ui/native-click.hpp"
#include "ui/native-style.hpp"
#include <array>
#include <algorithm>
#include <thread>
#include <cmath>
#include <limits>

namespace {
using namespace managed;
constexpr const char* Ui="UnityEngine.UIElementsModule.dll";
constexpr const char* UiSpace="UnityEngine.UIElements";
constexpr const char* Figma="Core.UILogic.Components.Figma";
struct Request {
    jobsxp::Query query;
    uint64_t generation=0;
    std::atomic_bool cancelled{false},done{false};
    jobsxp::Page page;
    std::string error;
};
struct ItemView {
    Ref tile,line,label,visual;
    int id=0;
    int64_t quantity=0;
    bool bound=false;
};
struct State {
    Ref owner,handler,root,tabs,content,tab,pane,cached,compositor;
    Ref recipes,resources,recipeTab,resourceTab,tabGroup,recipesDisplay,resourcesDisplay,nativeRecipes;
    Ref title,status,target,coefficient,epic,refresh,previous,next,details,ingredientList;
    std::array<Ref,jobsxp::PageSize> rows,recipeLines;
    std::array<ItemView,jobsxp::PageSize> recipeItems;
    std::vector<ItemView> ingredients;
    jobsxp::Page page;
    jobsxp::Query query;
    std::shared_ptr<Request> request;
    int64_t xp=0;
    int selected=-1,lastTarget=0,lastCoefficient=0;
    std::array<float,8> tabMetrics{};
    bool haveTabMetrics=false;
    float tabTranslation=std::numeric_limits<float>::quiet_NaN();
    bool lastEpic=false,active=false,cleanup=false,havePage=false,requested=false,recipeWasOn=false,resourceWasOn=false;
    ULONGLONG nextPoll=0,nextRequest=0;
    uint64_t generation=0;
} state;
Ref pending;
std::shared_ptr<Request> worker;
uint64_t generation=0;
bool installed=false;
Method createItem=nullptr,bindTile=nullptr,clearTileImage=nullptr,clearTileEvents=nullptr;
void(*originalEnable)(Obj,Method)=nullptr;
void(*originalDisable)(Obj,Method)=nullptr;
void(*originalSwitch)(Obj,int,Method)=nullptr;

bool Session() {return module&&module->ready&&module->servicesReady&&!module->stopped&&!module->stopRequested;}
Obj Style(Obj element,const char* property) {
    Ref style(api.Call(element,"get_style"));
    return api.Call(style.get(),(std::string("UnityEngine.UIElements.IStyle.get_")+property).c_str());
}
void WriteStyle(Obj element,const char* property,Obj box) {
    Ref style(api.Call(element,"get_style"));
    api.Call(style.get(),(std::string("UnityEngine.UIElements.IStyle.set_")+property).c_str(),{api.object_unbox(box)});
}
void Enum(Obj element,const char* property,int value) {
    Ref box(Style(element,property));api.Call(box.get(),"set_value",{&value});WriteStyle(element,property,box.get());
}
void Float(Obj element,const char* property,float value) {
    Ref box(Style(element,property));api.Call(box.get(),"set_value",{&value});WriteStyle(element,property,box.get());
}
void Pixels(Obj element,const char* property,float value) {
    int unit=0;Ref length(api.New(api.Class(Ui,UiSpace,"Length"),{&value,&unit}));
    Ref box(Style(element,property));api.Call(box.get(),"set_value",{api.object_unbox(length.get())});WriteStyle(element,property,box.get());
}
void Text(Obj element,const std::string& text,const char* setter="set_text") {
    Ref value(api.string_new(text.c_str()));api.Call(element,setter,{value.get()});
}
void Add(Obj parent,Obj child) {api.Call(parent,"Add",{child});}
void Enabled(Obj element,bool value){api.Call(element,"SetEnabled",{&value});}
// The game's label and text styles, which follow the chosen theme.
Obj Label(Obj parent,const std::string& text,bool secondary=false) {
    Ref label(nativeStyle::NewLabel());
    Enum(label.get(),"whiteSpace",0);Pixels(label.get(),"marginBottom",secondary?0.f:4.f);
    if(secondary)nativeStyle::Secondary(label.get());else nativeStyle::Body(label.get());
    Text(label.get(),text);Add(parent,label.get());return label.get();
}
Obj Row(Obj parent) {
    Ref row(api.New(api.Class(Ui,UiSpace,"VisualElement")));Enum(row.get(),"flexDirection",2);
    Enum(row.get(),"alignItems",2);Enum(row.get(),"flexWrap",1);
    Pixels(row.get(),"marginBottom",8);Add(parent,row.get());return row.get();
}
// The game's button: its hover, pressed and disabled states, and its theme.
// Styles: ComponentStyleEnum secondary 1, tertiary 2 (the flat paging arrows).
constexpr int SecondaryStyle=1,TertiaryStyle=2,IconPrevious=44,IconNext=45;
Obj Button(Obj parent,const std::string& text,int style,int icon,const char* tooltip) {
    Ref button(api.New(api.Class("Core.dll",Figma,"DofusButtonCustom")));
    int size=0;api.Call(button.get(),"set_size",{&size});api.Call(button.get(),"set_mainStyle",{&style});
    if(icon)api.Call(button.get(),"set_icon",{&icon});
    if(!text.empty())Text(button.get(),text);
    Text(button.get(),tooltip,"set_tooltip");Float(button.get(),"flexShrink",0);
    Add(parent,button.get());return button.get();
}
// A recipe: the game's selectable list row, its selection shown by its own activated state.
Obj RecipeRow(Obj parent) {
    Ref row(api.New(api.Class("Core.dll",Figma,"ListElementBasic")));
    Ref label(api.Field(row.get(),"m_textLabel"));
    bool no=false;api.Call(label.get(),"set_enableRichText",{&no});api.Call(label.get(),"set_isSingleLine",{&no});
    Enum(label.get(),"whiteSpace",0);
    int choice=0;api.Call(row.get(),"set_listElementStyle",{&choice});
    Pixels(row.get(),"minHeight",56);Pixels(row.get(),"minWidth",0);Float(row.get(),"flexGrow",1);
    Add(parent,row.get());return row.get();
}
Obj Input(Obj parent,const std::string& value,const char* tooltip,int maxLength) {
    Ref input(api.New(api.Class("Core.dll",Figma,"TextInput")));
    Text(input.get(),value);Text(input.get(),tooltip,"set_tooltip");api.Call(input.get(),"set_maxLength",{&maxLength});
    Pixels(input.get(),"width",88);Pixels(input.get(),"minWidth",0);Pixels(input.get(),"marginRight",12);Add(parent,input.get());return input.get();
}
void TabValue(Obj tab,bool value){api.Call(tab,"SetValueWithoutNotify",{&value});}
// The game's titled section, opened then locked: set_isOpen keeps the closed state
// while canBeOpened is off, which it is after the constructor.
Obj Section(Obj parent,const char* title) {
    bool subContent=true,folderButton=true,no=false,noIcon=false,open=true,openable=true,locked=false;
    Ref section(api.New(api.Class("Core.dll",Figma,"SectionHeader"),{&subContent,&folderButton,&no,&noIcon,&no}));
    Text(section.get(),title,"set_title");
    api.Call(section.get(),"set_canBeOpened",{&openable});api.Call(section.get(),"set_isOpen",{&open});api.Call(section.get(),"set_canBeOpened",{&locked});
    Ref content(api.New(api.Class(Ui,UiSpace,"VisualElement")));
    for(const char* property:{"paddingLeft","paddingRight"})Pixels(content.get(),property,4);
    Pixels(content.get(),"paddingTop",8);Pixels(content.get(),"paddingBottom",4);
    api.Call(section.get(),"AddSubContent",{content.get()});
    Pixels(section.get(),"marginBottom",8);
    Add(parent,section.get());return content.get();
}
void MatchTabLayout() {
    // Read resolved geometry: UXML/USS values are not necessarily inline styles.
    Ref resolved(api.Call(state.resourceTab.get(),"get_resolvedStyle"));
    Ref preceding(api.Call(state.recipeTab.get(),"get_resolvedStyle"));
    const std::array<const char*,7> properties={"height","bottom","marginTop","marginBottom","paddingTop","paddingBottom","marginLeft"};
    std::array<float,8> values{};
    for(size_t i=0;i<properties.size();++i)
        values[i]=api.Value<float>(api.Call(resolved.get(),(std::string("get_")+properties[i]).c_str()));
    // Include either sibling's margins so the two inter-tab spaces are identical.
    values[6]+=api.Value<float>(api.Call(preceding.get(),"get_marginRight"))-
        api.Value<float>(api.Call(resolved.get(),"get_marginRight"));
    values[7]=static_cast<float>(api.Value<int>(api.Call(resolved.get(),"get_alignSelf")));
    if(!std::isfinite(values[0])||values[0]<16||values[0]>128)return;
    for(size_t i=0;i<values.size();++i) {
        if(!std::isfinite(values[i]))continue; // Unresolved auto offsets retain the sibling's inline value.
        if(state.haveTabMetrics&&std::abs(values[i]-state.tabMetrics[i])<.01f)continue;
        if(i==7)Enum(state.tab.get(),"alignSelf",static_cast<int>(values[i]));
        else Pixels(state.tab.get(),properties[i],values[i]);
        if(i==0)for(const char* property:{"minHeight","maxHeight"})Pixels(state.tab.get(),property,values[i]);
    }
    state.tabMetrics=values;state.haveTabMetrics=true;
    // Inline/style metrics alone do not guarantee the final sibling positions.
    // Layout rectangles share ctr_tabs coordinates and exclude transforms, so
    // this correction cannot accumulate or feed back into the flex layout.
    struct Rect {float x,y,width,height;};
    const auto reference=api.Value<Rect>(api.Call(state.resourceTab.get(),"get_layout"));
    const auto actual=api.Value<Rect>(api.Call(state.tab.get(),"get_layout"));
    const float offset=reference.y-actual.y;
    if(!std::isfinite(offset)||!std::isfinite(reference.height)||!std::isfinite(actual.height)||
        reference.height<16||actual.height<16||reference.width<=0||actual.width<=0)return;
    if(std::isfinite(state.tabTranslation)&&std::abs(offset-state.tabTranslation)<.01f)return;
    struct Vector3 {float x,y,z;} translation{0,offset,0};
    Ref transform(api.Call(state.tab.get(),"get_transform"));
    api.Call(transform.get(),"UnityEngine.UIElements.ITransform.set_position",{&translation});
    state.tabTranslation=offset;
}
void ClearItem(ItemView& view) {
    if(view.tile.get()) {
        // Native recipe recycling removes only this tile's registered manipulators.
        // Keep ownership until every step succeeds so partial cleanup is retryable.
        api.gc_wbarrier_set_field(view.tile.get(),static_cast<void**>(api.FieldAddress(view.tile.get(),"onRightClickCallback")),nullptr);
        api.Invoke(clearTileEvents,nullptr,{view.tile.get()});
        api.Invoke(clearTileImage,nullptr,{view.tile.get()});
        api.Call(view.tile.get(),"RemoveFromHierarchy");
        view.tile.reset();
    }
    view.id=0;view.quantity=0;view.bound=false;
}
void ClearIngredients() {
    for(auto& view:state.ingredients) {
        ClearItem(view);
        if(view.line.get())api.Call(view.line.get(),"RemoveFromHierarchy");
    }
    state.ingredients.clear();
}
void ClearItems() {
    for(auto& view:state.recipeItems)ClearItem(view);
    ClearIngredients();
}
bool BindItem(ItemView& view,Obj parent,int id,int64_t quantity) {
    if(view.bound&&view.id==id&&view.quantity==quantity)return true;
    ClearItem(view);
    int16_t position=63;uint32_t uid=0,one=1;
    Ref item(api.Invoke(createItem,nullptr,{&id,&position,&uid,&one,nullptr}));
    // Native factory substitutes its placeholder for an unknown DofusDB id.
    Ref data(item.get()?api.Call(item.get(),"myx"):nullptr);
    if(!data.get()||api.Value<int>(api.Field(data.get(),"id"))!=id)return false;
    view.tile.reset(api.New(api.Class("Core.dll","Core.UILogic.Components.Slots","Tile")));
    int medium=2;api.Call(view.tile.get(),"set_tileSize",{&medium});
    Pixels(view.tile.get(),"marginRight",10);Float(view.tile.get(),"flexShrink",0);
    Add(parent,view.tile.get());
    // Native recipe binding provides the image, hover tooltip and usual item actions.
    // Its quantity is Int32; retain the full planner total in both visible counters.
    int nativeQuantity=static_cast<int>(std::min<int64_t>(quantity,std::numeric_limits<int>::max()));
    api.Invoke(bindTile,state.nativeRecipes.get(),{item.get(),view.tile.get(),&nativeQuantity});
    Text(view.tile.get(),std::to_string(quantity),"set_counterValue");
    view.id=id;view.quantity=quantity;view.bound=true;
    return true;
}
void RenderIngredients(const jobsxp::Recipe& recipe,int count) {
    if(count<=0){ClearIngredients();return;}
    if(state.ingredients.size()!=recipe.ingredients.size()) {
        ClearIngredients();state.ingredients.resize(recipe.ingredients.size());
    }
    for(size_t i=0;i<recipe.ingredients.size();++i) {
        auto& view=state.ingredients[i];const auto& ingredient=recipe.ingredients[i];
        if(!view.line.get()) {
            view.line.reset(Row(state.ingredientList.get()));Enum(view.line.get(),"flexWrap",0);
            view.visual.reset(api.New(api.Class(Ui,UiSpace,"VisualElement")));Add(view.line.get(),view.visual.get());
            view.label.reset(Label(view.line.get(),""));Float(view.label.get(),"flexGrow",1);Pixels(view.label.get(),"minWidth",0);
        }
        // Keep native item first and the total/name alongside it.
        const int64_t total=static_cast<int64_t>(ingredient.quantity)*count;
        const bool available=BindItem(view,view.visual.get(),ingredient.id,total);
        const auto stock=recipeNative::Owned(ingredient.id);
        const auto owned=stock.Total();
        const auto missing=recipeCost::Missing(total,owned);
        std::string text=std::to_string(total)+" × "+ingredient.name+(available?"":" · objet indisponible dans cette version");
        if(stock.bagKnown){
            text+="\nSac "+std::to_string(stock.bag)+(stock.bankKnown?" · Banque "+std::to_string(stock.bank):" · Banque non lue")+
                (stock.havenBagKnown?" · Havre-sac "+std::to_string(stock.havenBag):"");
            text+=missing?" · il en manque "+std::to_string(missing):" · tu as tout";
        }
        const auto price=recipeNative::AveragePrice(ingredient.id);
        if(price>0)text+=(stock.bagKnown?" · ":"\n")+recipeCost::Kamas(price*total);
        Text(view.label.get(),text);
    }
}
bool Current() {
    return Session()&&state.pane.get()&&!state.cleanup&&state.cached.get()==module->cached.get()&&
        state.compositor.get()==module->compositor.get()&&
        api.Field(state.owner.get(),"m_jobCraftAndSkillsHandler")==state.handler.get()&&
        api.Field(state.handler.get(),"m_jobWindowRecipesUI")==state.nativeRecipes.get()&&
        api.Field(state.nativeRecipes.get(),"m_currentRecipesFilter")&&
        api.Call(state.pane.get(),"get_parent")==state.content.get()&&api.Call(state.tab.get(),"get_parent")==state.tabs.get()&&
        api.Call(state.root.get(),"get_panel");
}
void Cancel() {
    if(state.request)state.request->cancelled.store(true);
    state.request.reset();state.requested=false;
}
void Leave(bool restoreSelection=true) {
    if(!state.active)return;
    Enum(state.pane.get(),"display",1); // Hide interactive items before any retryable native cleanup.
    ClearItems();
    // Restore exact original inline display styles before native tab switching.
    if(state.recipesDisplay.get())WriteStyle(state.recipes.get(),"display",state.recipesDisplay.get());
    if(state.resourcesDisplay.get())WriteStyle(state.resources.get(),"display",state.resourcesDisplay.get());
    if(restoreSelection){
        // Activate the saved native tab first: the group deliberately disallows an empty selection.
        if(state.recipeWasOn)TabValue(state.recipeTab.get(),true);
        else if(state.resourceWasOn)TabValue(state.resourceTab.get(),true);
    }
    Enum(state.pane.get(),"display",1);TabValue(state.tab.get(),false);
    state.recipesDisplay.reset();state.resourcesDisplay.reset();state.active=false;Cancel();
}
void Render();
void UpdateCharacter();
void Enter() {
    if(state.active){TabValue(state.tab.get(),true);return;}
    state.recipesDisplay.reset(Style(state.recipes.get(),"display"));
    state.resourcesDisplay.reset(Style(state.resources.get(),"display"));
    state.recipeWasOn=api.Value<bool>(api.Call(state.recipeTab.get(),"get_isOn"));
    state.resourceWasOn=api.Value<bool>(api.Call(state.resourceTab.get(),"get_isOn"));
    state.active=true; // Cleanup owns partially applied changes from here.
    Enum(state.recipes.get(),"display",1);Enum(state.resources.get(),"display",1);
    // Native API temporarily allows an empty group and restores its policy afterwards.
    // SetValueWithoutNotify(false) alone refuses to clear its final selected item.
    api.Call(state.tabGroup.get(),"ForceAllSelectionsOff");TabValue(state.tab.get(),true);
    Enum(state.pane.get(),"display",0);
    UpdateCharacter();Render();state.nextPoll=GetTickCount64()+500;
}
bool Parameters(int& target,int& coefficient,bool& epic) {
    epic=api.Value<bool>(api.Call(state.epic.get(),"get_value"));
    return jobsxp::Number(api.String(api.Call(state.target.get(),"get_text")),1,200,target)&&
        jobsxp::Number(api.String(api.Call(state.coefficient.get(),"get_text")),1,600,coefficient);
}
void Render() {
    int target=0,coefficient=0;bool epic=false;
    const bool valid=Parameters(target,coefficient,epic);
    for(size_t i=0;i<state.rows.size();++i) {
        const bool show=state.havePage&&i<state.page.recipes.size();
        Enum(state.recipeLines[i].get(),"display",show?0:1);
        if(!show){ClearItem(state.recipeItems[i]);continue;}
        const auto& r=state.page.recipes[i];
        const bool available=BindItem(state.recipeItems[i],state.recipeItems[i].visual.get(),r.id,1);
        std::string caption=r.name+" · niv. "+std::to_string(r.level)+(available?"":" · indisponible dans cette version");
        if(valid) {
            const auto plan=jobsxp::Simulate(r,state.xp,target,coefficient,epic);
            caption+="\n"+std::to_string(jobsxp::CraftXp(r,state.query.level,coefficient,epic))+" XP · ";
            caption+=plan.reached?"× "+std::to_string(plan.count):"niveau visé hors de portée";
        }
        Text(state.rows[i].get(),caption);
        bool activated=state.selected==static_cast<int>(i);api.Call(state.rows[i].get(),"set_isActivated",{&activated});
    }
    Enabled(state.previous.get(),state.query.skip>0&&!state.request);
    Enabled(state.next.get(),state.havePage&&state.query.skip+jobsxp::PageSize<state.page.total&&!state.request);
    if(!valid){ClearIngredients();Text(state.details.get(),"Niveau : 1 à 200. Bonus d'XP : 1 à 600 %.");}
    else if(state.havePage&&state.selected>=0&&state.selected<static_cast<int>(state.page.recipes.size())) {
        const auto& r=state.page.recipes[state.selected];
        const auto plan=jobsxp::Simulate(r,state.xp,target,coefficient,epic);
        std::string text;
        RenderIngredients(r,plan.count);
        if(plan.count==0&&plan.reached)text="Niveau déjà atteint.";
        else {
            text=std::to_string(plan.count)+" × "+r.name+" : +"+std::to_string(plan.xp-state.xp)+" XP, niveau "+std::to_string(jobsxp::Level(plan.xp));
            std::vector<recipeCost::Line> lines;
            for(const auto& ingredient:r.ingredients)lines.push_back({ingredient.quantity,recipeNative::AveragePrice(ingredient.id)});
            const auto cost=recipeCost::Sum(lines,plan.count);
            if(cost.cost>0)text+="\nCoût moyen : "+recipeCost::Kamas(cost.cost)+(cost.unpriced?" (sans "+std::to_string(cost.unpriced)+" ingrédient(s) sans prix)":"");
            if(!plan.reached)text+="\nCette recette ne suffit pas : choisis-en une autre.";
        }
        Text(state.details.get(),text);
    } else {ClearIngredients();Text(state.details.get(),"Choisis une recette.");}
    state.lastTarget=target;state.lastCoefficient=coefficient;state.lastEpic=epic;
}
void Search() {
    if(state.request||!jobsxp::Valid(state.query)||worker||GetTickCount64()<state.nextRequest)return;
    auto request=std::make_shared<Request>();request->query=state.query;request->generation=state.generation;
    state.request=worker=request;state.requested=true;state.nextRequest=GetTickCount64()+1500;
    Text(state.status.get(),"Chargement…");
    try {
        std::thread([request] {
            try {request->page=jobsxp::Fetch(request->query,request->cancelled);}
            catch(const std::exception& error){request->error=error.what();}
            catch(...){request->error="DofusDB : erreur de recherche.";}
            request->done.store(true);
        }).detach();
    }catch(...){state.request.reset();worker.reset();state.requested=false;throw;}
}
bool Action(Obj item) {
    const bool owned=item&&(item==state.refresh.get()||item==state.previous.get()||item==state.next.get()||
        std::any_of(state.rows.begin(),state.rows.end(),[&](const Ref& row){return item==row.get();}));
    if(!owned)return false;
    if(!Current()||!state.active)return true;
    if(item==state.refresh.get()) {Cancel();state.havePage=false;Search();Render();return true;}
    if(item==state.previous.get()||item==state.next.get()) {
        if(state.request)return true;
        const int skip=state.query.skip+(item==state.previous.get()?-jobsxp::PageSize:jobsxp::PageSize);
        if(skip<0||skip>=state.page.total)return true;
        state.query.skip=skip;state.selected=-1;state.havePage=false;state.requested=false;
        Render();Search();return true;
    }
    for(size_t i=0;i<state.rows.size();++i)if(item==state.rows[i].get()){
        if(state.havePage&&i<state.page.recipes.size()){state.selected=static_cast<int>(i);Render();}
        return true;
    }
    return false;
}
void Create(Obj owner) {
    state.owner.reset(owner);state.handler.reset(api.Field(owner,"m_jobCraftAndSkillsHandler"));
    if(!state.handler.get()||api.object_get_class(state.handler.get())!=api.Class("Core.dll","Core.UILogic.Job","JobCraftAndSkillsHandler"))
        throw std::runtime_error("Jobs handler unavailable");
    Ref binding(api.Field(state.handler.get(),"m_binding"));
    state.root.reset(api.Call(binding.get(),"GetRootElement"));
    if(!state.root.get()||!api.Call(state.root.get(),"get_panel")){state=State{};return;}
    state.tabs.reset(api.Call(binding.get(),"get_ctr_tabs"));state.content.reset(api.Call(binding.get(),"get_content"));
    state.recipeTab.reset(api.Call(binding.get(),"get_tab_recipe"));state.resourceTab.reset(api.Call(binding.get(),"get_tab_ressources"));
    state.tabGroup.reset(api.Call(state.recipeTab.get(),"get_group"));
    if(!state.tabGroup.get()||state.tabGroup.get()!=api.Call(state.resourceTab.get(),"get_group"))
        throw std::runtime_error("Jobs native tab group unavailable");
    state.recipes.reset(api.Call(binding.get(),"get_recipesUI"));state.resources.reset(api.Call(binding.get(),"get_jobSkillsUI"));
    state.nativeRecipes.reset(api.Field(state.handler.get(),"m_jobWindowRecipesUI"));
    if(!state.nativeRecipes.get()){state=State{};return;}
    if(api.object_get_class(state.nativeRecipes.get())!=api.Class("Core.dll","Core.UILogic.Crafting.Recipes","JobWindowRecipesUI"))
        throw std::runtime_error("Unsupported native recipe item binding");
    // OnEnable may precede the native recipe filter's initialization.
    if(!api.Field(state.nativeRecipes.get(),"m_currentRecipesFilter")){state=State{};return;}
    state.cached.reset(module->cached.get());state.compositor.reset(module->compositor.get());state.generation=++generation;
    state.tab.reset(api.New(api.Class("Core.dll",Figma,"DofusTab")));Text(state.tab.get(),"XP");
    int size=api.Value<int>(api.Call(state.recipeTab.get(),"get_size"));api.Call(state.tab.get(),"set_size",{&size});
    int fontCase=api.Value<int>(api.Call(state.recipeTab.get(),"get_fontCase"));api.Call(state.tab.get(),"set_fontCase",{&fontCase});Text(state.tab.get(),"XP");
    for(const char* property:{"bottom","top","marginTop","marginBottom","marginLeft","alignSelf"}) {
        Ref value(Style(state.resourceTab.get(),property));WriteStyle(state.tab.get(),property,value.get());
    }
    Float(state.tab.get(),"flexGrow",1);Pixels(state.tab.get(),"minWidth",0);
    Text(state.tab.get(),"Ce qu'il faut fabriquer pour atteindre un niveau.","set_tooltip");
    int vertical=0;state.pane.reset(api.New(api.Class(Ui,UiSpace,"ScrollView"),{&vertical}));
    int hidden=2,automatic=0;api.Call(state.pane.get(),"set_horizontalScrollerVisibility",{&hidden});
    api.Call(state.pane.get(),"set_verticalScrollerVisibility",{&automatic});
    Float(state.pane.get(),"flexGrow",1);Pixels(state.pane.get(),"minHeight",0);Enum(state.pane.get(),"display",1);
    Ref body(api.Call(state.pane.get(),"get_contentContainer"));
    for(const char* property:{"paddingLeft","paddingRight","paddingTop","paddingBottom"})Pixels(body.get(),property,8);
    Ref goal(Section(body.get(),"Objectif"));
    state.title.reset(Label(goal.get(),"Métier"));
    nativeStyle::Strong(state.title.get());
    Ref controls(Row(goal.get()));
    Ref targetName(Label(controls.get(),"Niveau visé"));Pixels(targetName.get(),"marginRight",8);Pixels(targetName.get(),"marginBottom",0);
    state.target.reset(Input(controls.get(),"200","Le niveau à atteindre, de 1 à 200.",3));
    Ref bonusName(Label(controls.get(),"Bonus d'XP (%)"));Pixels(bonusName.get(),"marginRight",8);Pixels(bonusName.get(),"marginBottom",0);
    state.coefficient.reset(Input(controls.get(),"100","100 % sans bonus. Compte ici les bonus d'XP de fabrication, comme celui de l'Almanax.",3));
    state.epic.reset(api.New(api.Class("Core.dll",Figma,"CheckboxCustom")));
    Text(state.epic.get(),"Serveur Épique","set_rightText");Add(goal.get(),state.epic.get());
    Ref list(Section(body.get(),"Recettes"));
    for(size_t i=0;i<state.rows.size();++i){
        auto& row=state.rows[i];state.recipeLines[i].reset(Row(list.get()));Enum(state.recipeLines[i].get(),"flexWrap",0);
        Pixels(state.recipeLines[i].get(),"marginBottom",2);
        state.recipeItems[i].visual.reset(api.New(api.Class(Ui,UiSpace,"VisualElement")));Add(state.recipeLines[i].get(),state.recipeItems[i].visual.get());
        row.reset(RecipeRow(state.recipeLines[i].get()));Enum(state.recipeLines[i].get(),"display",1);
    }
    Ref pages(Row(list.get()));Enum(pages.get(),"flexWrap",0);Pixels(pages.get(),"marginTop",6);Pixels(pages.get(),"marginBottom",0);
    state.previous.reset(Button(pages.get(),"",TertiaryStyle,IconPrevious,"Recettes précédentes"));
    state.status.reset(Label(pages.get(),"",true));Float(state.status.get(),"flexGrow",1);Pixels(state.status.get(),"marginBottom",0);
    Enum(state.status.get(),"unityTextAlign",4);
    state.next.reset(Button(pages.get(),"",TertiaryStyle,IconNext,"Recettes suivantes"));
    state.refresh.reset(Button(pages.get(),"Actualiser",SecondaryStyle,0,"Relit les recettes sur DofusDB."));Pixels(state.refresh.get(),"marginLeft",8);
    Ref result(Section(body.get(),"Ingrédients"));
    state.details.reset(Label(result.get(),"Choisis une recette."));
    state.ingredientList.reset(api.New(api.Class(Ui,UiSpace,"VisualElement")));Add(result.get(),state.ingredientList.get());
    Add(state.tabs.get(),state.tab.get());Add(state.content.get(),state.pane.get());
    Log("JOBS_XP_PANEL_READY nativeTab=true provider=DofusDB");
}
void UpdateCharacter() {
    int id=api.Value<int>(api.Field(state.handler.get(),"m_jobId"));
    Ref service(module->FindService("fmp"));
    if(!service.get()||api.object_get_class(service.get())!=api.Class("Core.dll","","fag"))
        throw std::runtime_error("Jobs service unavailable");
    Ref job(api.Call(service.get(),"bbxi",{&id}));
    if(!job.get()||api.object_get_class(job.get())!=api.Class("Core.dll","","ll"))
        throw std::runtime_error("Selected job unavailable");
    const int level=api.Value<int>(api.Call(job.get(),"obv"));
    const auto xp=api.Value<int64_t>(api.Call(job.get(),"obx"));
    if(level<1||level>200||xp<0||jobsxp::Level(xp)!=level)throw std::runtime_error("Jobs experience inconsistent");
    const bool changed=state.query.job!=id||state.query.level!=level;
    if(changed){
        const bool newJob=state.query.job!=id;
        Cancel();state.query={id,level,0};state.havePage=false;state.selected=-1;
        if(newJob)Text(state.target.get(),std::to_string(std::min(200,((level/(id==79?5:10))+1)*(id==79?5:10))));
    }
    const bool xpChanged=state.xp!=xp;state.xp=xp;
    Text(state.title.get(),api.String(api.Call(job.get(),"obt"))+" · niveau "+std::to_string(level)+" · "+std::to_string(xp)+" XP");
    if(changed||xpChanged)Render();
}
void EnableHook(Obj owner,Method method){
    originalEnable(owner,method);
    try{if(Session()){RestoreJobsXp();if(!state.cleanup)pending.reset(owner);}}
    catch(const std::exception& e){Log(std::string("JOBS_XP_ENABLE_ERROR ")+e.what());}
}
void DisableHook(Obj owner,Method method){
    try{if(pending.get()==owner||state.owner.get()==owner)RestoreJobsXp();}
    catch(const std::exception& e){Log(std::string("JOBS_XP_DISABLE_ERROR ")+e.what());}
    originalDisable(owner,method);
}
void SwitchHook(Obj handler,int index,Method method){
    // Native receives only its original indices. Our tab is outside its group.
    try{if(state.handler.get()==handler){
        Leave(false);
        if(index==0)TabValue(state.recipeTab.get(),true);
        else if(index==1)TabValue(state.resourceTab.get(),true);
        if(state.tab.get())TabValue(state.tab.get(),false);
    }}
    catch(const std::exception& e){state.cleanup=true;Log(std::string("JOBS_XP_SWITCH_ERROR ")+e.what());}
    originalSwitch(handler,index,method);
}
void FieldContract(Klass klass,const char* name,size_t offset){
    const auto field=api.class_get_field_from_name(klass,name);
    if(!field||api.field_get_offset(field)!=offset)throw std::runtime_error(std::string("Jobs field layout: ")+name);
}
}
void InstallJobsXp(){
    if(installed)return;
    const auto ui=api.Class("Core.dll","Core.UILogic.Job","JobUI");
    const auto handler=api.Class("Core.dll","Core.UILogic.Job","JobCraftAndSkillsHandler");
    FieldContract(ui,"m_jobCraftAndSkillsHandler",0x150);
    FieldContract(handler,"m_binding",0x68);FieldContract(handler,"m_jobId",0x60);
    FieldContract(handler,"m_jobWindowRecipesUI",0x50);
    const auto recipes=api.Class("Core.dll","Core.UILogic.Crafting.Recipes","RecipesUI");
    FieldContract(recipes,"m_currentRecipesFilter",0x208);
    bindTile=api.FindVerified(recipes,"BindTileData",3,0x2D19E30);
    const auto item=api.Class("Core.dll","","mc");
    createItem=api.FindVerified(item,"ohc",5,0x2A6BF80);
    api.FindVerified(item,"myx",0,0x2A69C20);
    FieldContract(api.Class("Ankama.Dofus.Core.DataCenter.dll","Core.DataCenter.Metadata.Item","ItemData"),"id",0x14);
    const auto tile=api.Class("Core.dll","Core.UILogic.Components.Slots","Tile");
    FieldContract(tile,"onRightClickCallback",0x3F8);
    api.FindVerified(tile,".ctor",0,0x165E6A0);api.FindVerified(tile,"set_tileSize",1,0x165F7F0);
    api.FindVerified(tile,"set_counterValue",1,0x165EB50);
    const auto cleanup=api.Class("Core.dll","","pq");
    clearTileImage=api.FindVerified(cleanup,"paq",1,0x2E53F40);
    clearTileEvents=api.FindVerified(cleanup,"pbo",1,0x2E565F0);
    const auto binding=api.Class("Core.dll","Core.UILogic.Bindings","JobRecipesAndSkillsBinding");
    for(const auto& [name,rva]:std::array<std::pair<const char*,uintptr_t>,7>{{
        {"GetRootElement",0x68D2F0},{"get_ctr_tabs",0x2F1A0B0},{"get_content",0x2F19DF0},
        {"get_tab_recipe",0x2F1A370},{"get_tab_ressources",0x2F1A420},{"get_recipesUI",0x2F1A2C0},{"get_jobSkillsUI",0x2F1A160}}})
        api.FindVerified(binding,name,0,rva);
    const auto tab=api.Class("Core.dll",Figma,"DofusTab");
    api.FindVerified(tab,".ctor",0,0x170C850);api.FindVerified(tab,"set_text",1,0x170D180);
    api.FindVerified(tab,"get_size",0,0x16C8310);api.FindVerified(tab,"set_size",1,0x170CFE0);
    api.FindVerified(tab,"get_isOn",0,0x170C9E0);api.FindVerified(tab,"SetValueWithoutNotify",1,0x170B400);
    api.FindVerified(tab,"get_fontCase",0,0x1700490);api.FindVerified(tab,"set_fontCase",1,0x170CA50);
    api.FindVerified(tab,"get_group",0,0x15575D0);
    api.FindVerified(api.Class("Core.dll","Core.UILogic.Components","SelectableGroup"),"ForceAllSelectionsOff",0,0x15BBA60);
    api.FindVerified(api.Class(Ui,UiSpace,"VisualElement"),"get_resolvedStyle",0,0x55BC620);
    const auto element=api.Class(Ui,UiSpace,"VisualElement");
    api.FindVerified(element,"get_layout",0,0x55BBC70);
    api.FindVerified(element,"get_transform",0,0x3116A10);
    api.FindVerified(element,"UnityEngine.UIElements.ITransform.set_position",1,0x55B6610);
    const auto resolved=api.Class(Ui,UiSpace,"ResolvedStyleAccess");
    for(const auto& [name,rva]:std::array<std::pair<const char*,uintptr_t>,9>{{
        {"get_height",0x559ED80},{"get_bottom",0x559EB60},{"get_marginTop",0x559EEE0},
        {"get_marginBottom",0x559EE50},{"get_marginLeft",0x559EE80},{"get_marginRight",0x559EEB0},
        {"get_paddingTop",0x559F0D0},{"get_paddingBottom",0x559F040},{"get_alignSelf",0x559E690}}})
        api.FindVerified(resolved,name,0,rva);
    api.FindVerified(api.Class("Core.dll","","fag"),"bbxi",1,0x1A11F90);
    const auto job=api.Class("Core.dll","","ll");
    api.FindVerified(job,"obv",0,0x7E6000);api.FindVerified(job,"obx",0,0x73BCA0);api.FindVerified(job,"obt",0,0x68D2D0);
    const auto input=api.Class("Core.dll",Figma,"TextInput");
    api.FindVerified(input,".ctor",0,0x1787B20);api.FindVerified(input,"get_text",0,0x1787C30);
    api.FindVerified(input,"set_text",1,0x1788AE0);api.FindVerified(input,"set_maxLength",1,0x1788840);
    const auto checkbox=api.Class("Core.dll",Figma,"CheckboxCustom");
    api.FindVerified(checkbox,".ctor",0,0x16FE760);api.FindVerified(checkbox,"get_value",0,0x16FEAE0);
    api.FindVerified(checkbox,"set_rightText",1,0x16FF020);
    const auto scroll=api.Class(Ui,UiSpace,"ScrollView");
    api.FindVerified(scroll,".ctor",1,0x5706FB0);api.FindVerified(scroll,"get_contentContainer",0,0x5707F20);
    api.FindVerified(scroll,"set_horizontalScrollerVisibility",1,0x57085C0);
    api.FindVerified(scroll,"set_verticalScrollerVisibility",1,0x5708FF0);
    // ListElementBasic contracts are verified by InstallSoulHelper first.
    const auto label=api.Class("Core.dll","Core.UILogic.Components","DofusLabel");
    api.FindVerified(label,".ctor",0,0x158D3E0);api.FindVerified(label,"set_text",1,0x158E010);api.FindVerified(label,"set_isSingleLine",1,0x158DA50);
    const auto button=api.Class("Core.dll",Figma,"DofusButtonCustom");
    api.FindVerified(button,".ctor",0,0x1707180);api.FindVerified(button,"set_icon",1,0x1707A70);api.FindVerified(button,"set_text",1,0x1708080);
    api.FindVerified(button,"set_mainStyle",1,0x1707CA0);api.FindVerified(button,"set_size",1,0x1707DC0);
    const auto section=api.Class("Core.dll",Figma,"SectionHeader");
    api.FindVerified(section,".ctor",5,0x173F9D0);api.FindVerified(section,"set_title",1,0x1740280);api.FindVerified(section,"set_isOpen",1,0x173FDC0);
    api.FindVerified(section,"set_canBeOpened",1,0x173FB60);api.FindVerified(section,"AddSubContent",1,0x173DE30);
    api.FindVerified(api.Class("Core.dll",Figma,"ListElementBasic"),"set_isActivated",1,0x172D6C0);
    AddHook(api.FindVerified(ui,"OnEnable",0,0x2AB8A30),0x2AB8A30,reinterpret_cast<void*>(EnableHook),reinterpret_cast<void**>(&originalEnable));
    AddHook(api.FindVerified(ui,"OnDisable",0,0x2AB89F0),0x2AB89F0,reinterpret_cast<void*>(DisableHook),reinterpret_cast<void**>(&originalDisable));
    AddHook(api.FindVerified(handler,"SwitchTab",1,0x2A7E9E0),0x2A7E9E0,reinterpret_cast<void*>(SwitchHook),reinterpret_cast<void**>(&originalSwitch));
    soul::SetAuxiliaryPanelAction(Action);nativeClick::AddHandler(Action);installed=true;
}
void RestoreJobsXp(){
    pending.reset();Cancel();
    if(!state.pane.get()&&!state.tab.get()){state=State{};return;}
    state.cleanup=true;
    try{
        Leave();ClearItems();
        if(state.tab.get())api.Call(state.tab.get(),"RemoveFromHierarchy");
        if(state.pane.get())api.Call(state.pane.get(),"RemoveFromHierarchy");
        state=State{};
    }catch(const std::exception& e){Log(std::string("JOBS_XP_CLEANUP_ERROR ")+e.what());}
}
bool JobsXpCleanupPending(){return state.cleanup;}
void TickJobsXp(){
    if(!installed)return;
    if(worker&&worker->done.load())worker.reset();
    try{
        if(!Session()||state.cleanup||(state.pane.get()&&!Current())){RestoreJobsXp();return;}
        if(pending.get()&&!state.pane.get()){
            Create(pending.get());if(state.pane.get())pending.reset();
        }
        if(!state.pane.get())return;
        MatchTabLayout();
        if(api.Value<bool>(api.Call(state.tab.get(),"get_isOn")))Enter();
        if(!state.active)return;
        TabValue(state.tab.get(),true);
        const auto now=GetTickCount64();
        if(now>=state.nextPoll){state.nextPoll=now+500;UpdateCharacter();if(state.selected>=0)Render();} // The bag changes while the tab is open.
        if(state.request&&state.request->done.load()){
            auto request=std::move(state.request);
            if(jobsxp::AcceptResult(state.generation,request->generation,state.query,request->query,request->cancelled.load())){
                if(request->error.empty()){
                    state.page=std::move(request->page);state.havePage=true;state.selected=-1;
                    Text(state.status.get(),state.page.recipes.empty()?"Aucune recette à ce niveau.":
                        std::to_string(state.query.skip+1)+" à "+std::to_string(state.query.skip+state.page.recipes.size())+" sur "+std::to_string(state.page.total));
                }else Text(state.status.get(),request->error);
                Render();
            }
        }
        if(!state.requested&&!state.havePage)Search();
        Enabled(state.refresh.get(),!state.request&&!worker&&now>=state.nextRequest);
        int target=0,coefficient=0;bool epic=false;
        Parameters(target,coefficient,epic);
        if(target!=state.lastTarget||coefficient!=state.lastCoefficient||epic!=state.lastEpic)Render();
    }catch(const std::exception& e){
        Log(std::string("JOBS_XP_PANEL_ERROR ")+e.what());RestoreJobsXp();
    }
}
