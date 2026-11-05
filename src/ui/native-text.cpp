#include "ui/native-text.hpp"
#include "core/module.hpp"
#include <vector>

namespace nativeText {
namespace {
using namespace managed;
constexpr const char* Ui = "UnityEngine.UIElementsModule.dll";
constexpr const char* UiSpace = "UnityEngine.UIElements";
// The style of a just attached element resolves over the next frames.
constexpr int FirstFrame = 2, LastFrame = 30;
constexpr size_t Limit = 256;
struct Pending { Ref label, reference; int attachedFrames = 0; int waiting = 0; };
struct Color { float r, g, b, a; };
std::vector<Pending> pending;

// The element itself or its first label, breadth first and bounded.
Obj Model(Obj reference) {
    const auto labelClass = api.Class(Ui, UiSpace, "Label");
    std::vector<Ref> queue;
    queue.emplace_back(reference);
    for (size_t i = 0; i < queue.size() && i < 64; ++i) {
        const auto current = queue[i].get();
        if (api.class_is_assignable_from(labelClass, api.object_get_class(current))) return current;
        Ref hierarchy(api.Call(current, "get_hierarchy"));
        const int count = api.Value<int>(api.Call(hierarchy.get(), "get_childCount"));
        for (int child = 0; child < count && child < 16; ++child) queue.emplace_back(api.Call(hierarchy.get(), "get_Item", {&child}));
    }
    return nullptr;
}
void WriteStyle(Obj element, const char* property, Obj box) {
    Ref style(api.Call(element, "get_style"));
    api.Call(style.get(), (std::string("UnityEngine.UIElements.IStyle.set_") + property).c_str(), {api.object_unbox(box)});
}
Obj ReadStyle(Obj element, const char* property) {
    Ref style(api.Call(element, "get_style"));
    return api.Call(style.get(), (std::string("UnityEngine.UIElements.IStyle.get_") + property).c_str());
}
// Whether the copy took: a model not styled yet has no visible color.
bool Apply(Obj label, Obj reference) {
    Ref model(Model(reference));
    if (!model.get()) return false;
    Ref resolved(api.Call(model.get(), "get_resolvedStyle"));
    const auto color = api.Value<Color>(api.Call(resolved.get(), "get_color"));
    const auto size = api.Value<float>(api.Call(resolved.get(), "get_fontSize"));
    if (color.a <= 0 || !(size > 0)) return false;
    Ref value(api.New(api.Class("UnityEngine.CoreModule.dll", "UnityEngine", "Color"), {const_cast<float*>(&color.r), const_cast<float*>(&color.g), const_cast<float*>(&color.b), const_cast<float*>(&color.a)}));
    Ref colorBox(ReadStyle(label, "color"));
    api.Call(colorBox.get(), "set_value", {api.object_unbox(value.get())}); WriteStyle(label, "color", colorBox.get());
    float pixels = size; int unit = 0;
    Ref length(api.New(api.Class(Ui, UiSpace, "Length"), {&pixels, &unit}));
    Ref sizeBox(ReadStyle(label, "fontSize"));
    api.Call(sizeBox.get(), "set_value", {api.object_unbox(length.get())}); WriteStyle(label, "fontSize", sizeBox.get());
    return true;
}
}

void Follow(Obj label, Obj reference) {
    if (!label || !reference) return;
    for (auto& item : pending) if (item.label.get() == label) { item.reference.reset(reference); item.attachedFrames = 0; item.waiting = 0; return; }
    if (pending.size() >= Limit) pending.erase(pending.begin());
    pending.push_back({});
    pending.back().label.reset(label); pending.back().reference.reset(reference);
}

void Tick() {
    for (size_t i = 0; i < pending.size();) {
        auto& item = pending[i];
        bool done = false;
        try {
            if (api.Call(item.label.get(), "get_panel") && api.Call(item.reference.get(), "get_panel")) {
                const int frame = ++item.attachedFrames;
                if (frame == FirstFrame || frame == LastFrame) Apply(item.label.get(), item.reference.get());
                done = frame >= LastFrame;
            } else done = ++item.waiting > 600; // Never shown: a closed tooltip, a closed window.
        } catch (const std::exception& error) {
            Log(std::string("NATIVE_TEXT_ERROR ") + error.what());
            done = true;
        }
        if (done) pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void Clear() { pending.clear(); }
}
