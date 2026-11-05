#pragma once
#include "core/runtime.hpp"

// A label of the mod takes the color and size a native text of the same place really
// has, once the game has styled it: the game's text colors come from its theme's
// style sheets, which a label created by code does not match, and a DofusLabel left
// alone is nearly black. The first label inside `reference` is the model.
namespace nativeText {
void Follow(managed::Obj label, managed::Obj reference);
void Tick();  // Each frame; nothing to do most of the time.
void Clear();
}
