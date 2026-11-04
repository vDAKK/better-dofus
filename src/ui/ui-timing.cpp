#include "ui/ui-timing.hpp"
#include "ui/ui-timing-policy.hpp"
#include "core/module.hpp"
#include <array>
#include <mutex>
#include <string>

namespace {
using namespace managed;
// gob`1, the base of the game's windows; one shared code for all its instantiations.
constexpr uintptr_t ConstructorRva = 0x3BBFF00, ToggleRva = 0x3BBFC50, AppearRva = 0x3BBF440;
using ConstructorFn = void (*)(Obj, Obj, Obj, Method);
using ToggleFn = void (*)(Obj, bool, Method);
using AppearFn = void (*)(Obj, Method);
ConstructorFn originalConstructor = nullptr;
ToggleFn originalToggle = nullptr;
AppearFn originalAppear = nullptr;
ui_timing::Recorder recorder;
std::mutex recorderMutex;
uint64_t lastFrame = 0;
bool installed = false;

uint64_t NowMs() {
    static const auto frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }();
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart * 1000 / frequency);
}
// Must not disturb the game: a failed measure is only a missing one.
void Record(const char* kind, Obj self, uint64_t start) {
    try {
        const auto end = NowMs();
        const auto type = self ? api.object_get_class(self) : nullptr;
        const auto name = type ? api.class_get_name(type) : nullptr;
        std::lock_guard<std::mutex> guard(recorderMutex);
        recorder.Event(start, std::string(kind) + ":" + (name ? name : "?"), end - start);
    } catch (...) {}
}
void ConstructorHook(Obj self, Obj root, Obj bus, Method method) {
    const auto start = NowMs();
    originalConstructor(self, root, bus, method);
    Record("build", self, start);
}
void ToggleHook(Obj self, bool visible, Method method) {
    const auto start = NowMs();
    originalToggle(self, visible, method);
    Record(visible ? "show" : "hide", self, start);
}
void AppearHook(Obj self, Method method) {
    const auto start = NowMs();
    originalAppear(self, method);
    Record("appear", self, start);
}
// The opening's starting point. Only its time is kept, whichever the key.
bool Pressed() {
    static std::array<bool, 256> down{};
    constexpr int ranges[][2] = {{VK_LBUTTON, VK_RBUTTON}, {VK_TAB, VK_TAB}, {VK_ESCAPE, VK_ESCAPE},
        {'0', '9'}, {'A', 'Z'}, {VK_F1, VK_F12}};
    bool pressed = false;
    for (const auto& range : ranges) {
        for (int key = range[0]; key <= range[1]; ++key) {
            const bool now = (GetAsyncKeyState(key) & 0x8000) != 0;
            pressed = pressed || (now && !down[key]);
            down[key] = now;
        }
    }
    return pressed;
}
template <class Install> void Try(const char* name, Install install) {
    try { install(); }
    catch (const std::exception& error) { Log(std::string("UI_TIMING_UNAVAILABLE ") + name + " " + error.what()); }
}
}

void InstallUiTiming() {
    Try("window", [] {
        // MapInfoUI overrides none of these: Find gives gob`1's shared code.
        const auto window = api.Class("Core.dll", "Core.UILogic.GameUICore", "MapInfoUI");
        Try("build", [&] { AddHook(api.Find(window, ".ctor", 2), ConstructorRva, reinterpret_cast<void*>(&ConstructorHook), reinterpret_cast<void**>(&originalConstructor)); });
        Try("toggle", [&] { AddHook(api.Find(window, "Toggle", 1), ToggleRva, reinterpret_cast<void*>(&ToggleHook), reinterpret_cast<void**>(&originalToggle)); });
        Try("appear", [&] { AddHook(api.Find(window, "AppearEffect", 0), AppearRva, reinterpret_cast<void*>(&AppearHook), reinterpret_cast<void**>(&originalAppear)); });
        installed = originalConstructor || originalToggle || originalAppear;
        Log(std::string("UI_TIMING_READY build=") + (originalConstructor ? "1" : "0") + " toggle=" + (originalToggle ? "1" : "0") + " appear=" + (originalAppear ? "1" : "0"));
    });
}

void UiTimingFrame() {
    if (!installed) return;
    const auto now = NowMs();
    const auto gap = lastFrame ? now - lastFrame : 0;
    lastFrame = now;
    const bool pressed = Pressed();
    std::optional<std::string> line;
    {
        std::lock_guard<std::mutex> guard(recorderMutex);
        if (pressed) recorder.Input(now);
        line = recorder.Frame(now, gap);
    }
    if (line) Log(*line);
}
