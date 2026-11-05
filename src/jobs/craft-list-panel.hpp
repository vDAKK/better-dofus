#pragma once
#include "core/runtime.hpp"

// The window of the planned crafts: what they still take, live, and a click on a
// resource showing it in the open auction house, through the game's own selection from
// a recipe.
void InstallCraftList();
void TickCraftList();
void RestoreCraftList();
bool CraftListCleanupPending();
void CloseCraftListOffWorld();
// Whether the player wants the list (the option of its top-bar button).
bool CraftListEnabled();
// Adds one craft of the item; opens the window on a first craft. Returns its count.
int AddToCraftList(int item);
int CraftListCount(int item);
// A click on the element reaches the native click handlers (native-click.hpp) with the
// element as its target. False if the game's parts could not be verified.
bool MakeClickable(managed::Obj element);
// As a click on an ingredient in the game's recipes. False while no auction house shows
// its buying tab.
bool ShowInAuctionHouse(int item);
