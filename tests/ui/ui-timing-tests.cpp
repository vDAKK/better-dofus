#include "ui/ui-timing-policy.hpp"
#include <iostream>
#include <stdexcept>

using ui_timing::Recorder;

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
    try {
        // A click, a window built in a long frame, then calm.
        {
            Recorder recorder;
            recorder.Input(1000);
            Check(!recorder.Frame(1010, 16), "Nothing to report before an event");
            recorder.Event(1020, "build:Map", 300);
            recorder.Event(1325, "show:Map", 5);
            Check(!recorder.Frame(1340, 320), "A long frame does not end the opening");
            Check(!recorder.Frame(1400, 16), "The opening waits for calm");
            const auto line = recorder.Frame(2900, 16);
            Check(line.has_value(), "Calm ends the opening");
            Check(*line == "UI_TIMING input_to_event=20 total=340 events=build:Map@20+300,show:Map@325+5 long_frames=@340:320", "Exact line");
            Check(!recorder.Frame(5000, 16), "The line is given once");
        }
        // Frames during an asynchronous load, before the window exists, are counted.
        {
            Recorder recorder;
            recorder.Input(1000);
            recorder.Frame(1100, 90);
            recorder.Event(1200, "show:Map", 1);
            const auto line = recorder.Frame(3000, 16);
            Check(line && line->find("input_to_event=200") != std::string::npos && line->find("long_frames=@100:90") != std::string::npos, "A load before the window counts");
        }
        // An event without a recent input stays measured, from itself; long frames before it do not count.
        {
            Recorder recorder;
            recorder.Input(1000);
            recorder.Frame(1050, 60);
            recorder.Event(1000 + ui_timing::InputWindowMs + 1, "show:Quest", 2);
            const auto line = recorder.Frame(9000, 16);
            Check(line && line->find("input_to_event=none") != std::string::npos && line->find("total=2 ") != std::string::npos &&
                line->find("long_frames=none") != std::string::npos, "An old input is not this opening's");
        }
        // Short frames are not listed; a long list stays bounded.
        {
            Recorder recorder;
            recorder.Event(0, "show:Map", 0);
            recorder.Frame(10, ui_timing::LongFrameMs - 1);
            for (size_t i = 0; i != ui_timing::MaxItems + 2; ++i) recorder.Event(20 + i, "show:X", 0);
            const auto line = recorder.Frame(10000, 16);
            Check(line && line->find("long_frames=none") != std::string::npos && line->find(",+3 ") != std::string::npos, "Bounded events, short frames left out");
        }
        // The log has a budget, and says when it ends.
        {
            Recorder recorder;
            uint64_t now = 0;
            std::string last;
            for (size_t i = 0; i != ui_timing::LineBudget; ++i) {
                recorder.Event(now, "show:X", 0);
                now += ui_timing::QuietMs + 1;
                const auto line = recorder.Frame(now, 16);
                Check(line.has_value(), "Within the budget");
                last = *line;
                now += 1;
            }
            Check(last.find("budget_ended=1") != std::string::npos, "The last line says so");
            recorder.Event(now, "show:X", 0);
            Check(!recorder.Frame(now + ui_timing::QuietMs + 1, 16), "Nothing after the budget");
        }
        std::cout << "PASS: one line per opening, input, long frames, old input, bounds, budget\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
