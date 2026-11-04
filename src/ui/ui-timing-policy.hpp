#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// How long the game's windows take to open: groups what happens around one
// opening into one log line. Pure: no game, no clock, no file. Only window class
// names and durations are kept, never which key was pressed.
namespace ui_timing {
inline constexpr uint64_t QuietMs = 1500, InputWindowMs = 3000, LongFrameMs = 25;
inline constexpr size_t LineBudget = 400, MaxItems = 48;

class Recorder {
public:
    void Input(uint64_t now) { lastInput = now; }
    void Event(uint64_t start, std::string_view name, uint64_t duration) {
        if (!lines) return;
        if (!active) {
            active = true;
            first = start;
            input = lastInput && start >= lastInput && start - lastInput <= InputWindowMs ? lastInput : 0;
            events.clear();
            dropped = 0;
        }
        last = std::max(last, start + duration);
        if (events.size() < MaxItems) events.push_back({start, std::string(name), duration});
        else ++dropped;
    }
    // Each frame. Returns the line once the opening is over.
    std::optional<std::string> Frame(uint64_t now, uint64_t gap) {
        if (gap >= LongFrameMs) {
            frames.push_back({now, gap});
            if (frames.size() > MaxItems) frames.pop_front();
        }
        if (!active || now < last || now - last <= QuietMs) return std::nullopt;
        active = false;
        return Close();
    }
private:
    struct Item { uint64_t at; std::string name; uint64_t duration; };
    struct LongFrame { uint64_t end, gap; };
    std::string Close() {
        // A long frame that ends after the input may have started before it.
        const auto base = input ? input : first;
        auto end = last;
        std::string longFrames;
        for (const auto& frame : frames) {
            if (frame.end <= base || frame.end > last + QuietMs) continue;
            end = std::max(end, frame.end);
            if (!longFrames.empty()) longFrames += ',';
            longFrames += "@" + std::to_string(frame.end - base) + ":" + std::to_string(frame.gap);
        }
        frames.clear();
        std::string line = "UI_TIMING input_to_event=" + (input ? std::to_string(first - input) : std::string("none")) +
            " total=" + std::to_string(end - base) + " events=";
        for (size_t i = 0; i != events.size(); ++i) {
            if (i) line += ',';
            line += events[i].name + "@" + std::to_string(events[i].at - base) + "+" + std::to_string(events[i].duration);
        }
        if (dropped) line += ",+" + std::to_string(dropped);
        line += " long_frames=" + (longFrames.empty() ? std::string("none") : longFrames);
        if (!--lines) line += " budget_ended=1";
        return line;
    }
    size_t lines = LineBudget, dropped = 0;
    bool active = false;
    uint64_t lastInput = 0, input = 0, first = 0, last = 0;
    std::vector<Item> events;
    std::deque<LongFrame> frames;
};
}
