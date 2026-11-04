#pragma once

// Measures how long the game's windows take to open, in module.log. Changes nothing
// in the game. Never throws: UI_TIMING_UNAVAILABLE only leaves a measure out.
void InstallUiTiming();
void UiTimingFrame();
