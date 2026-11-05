#pragma once

// The window of the planned crafts: what they still take, live, and a click showing a
// resource in the open auction house, through the game's own selection from a recipe.
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
