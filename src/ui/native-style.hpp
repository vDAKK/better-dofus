#pragma once
#include "core/runtime.hpp"
#include <string>

// The game's own text styles: the classes of its style sheets, the same ones its
// windows use (uielements bundle). They follow the chosen theme, unlike a color
// written by code, and a DofusLabel without them is nearly black. Every text of the
// mod goes through these, so that none of them can look different from the game's.
namespace nativeStyle {
inline constexpr const char* SizeBody = "textShort_mediumRegular";
inline constexpr const char* SizeStrong = "textShort_mediumBold";
inline constexpr const char* SizeSecondary = "textShort_smallRegular";
inline constexpr const char* SizeHeading = "textShort_smallBold";
inline constexpr const char* ColorBody = "textColor_white_white80";
inline constexpr const char* ColorStrong = "textColor_white_white100";
inline constexpr const char* ColorSecondary = "textColor_white_white65";
inline constexpr const char* ColorHeading = "textColor_highlight_secondary100";
inline constexpr const char* ColorPositive = "textColor_qualitative_positive";
inline constexpr const char* ColorNegative = "textColor_qualitative_negative";

inline void Add(managed::Obj element, const char* name) {
    managed::Ref value(managed::api.string_new(name));
    managed::api.Call(element, "AddToClassList", {value.get()});
}
inline void Toggle(managed::Obj element, const char* name, bool on) {
    managed::Ref value(managed::api.string_new(name));
    managed::api.Call(element, "EnableInClassList", {value.get(), &on});
}
inline void Body(managed::Obj label) { Add(label, SizeBody); Add(label, ColorBody); }
inline void Strong(managed::Obj label) { Add(label, SizeStrong); Add(label, ColorStrong); }
inline void Secondary(managed::Obj label) { Add(label, SizeSecondary); Add(label, ColorSecondary); }
inline void Heading(managed::Obj label) { Add(label, SizeHeading); Add(label, ColorHeading); }
// A text saying that something is right or wrong.
inline void Tone(managed::Obj label, bool positive, bool negative) {
    Toggle(label, ColorPositive, positive);
    Toggle(label, ColorNegative, negative);
    Toggle(label, ColorBody, !positive && !negative);
}
// The game's label, wrapping its text.
inline managed::Obj NewLabel() {
    managed::Ref label(managed::api.New(managed::api.Class("Core.dll", "Core.UILogic.Components", "DofusLabel")));
    bool multiline = false;
    managed::api.Call(label.get(), "set_isSingleLine", {&multiline});
    return label.get();
}
}
