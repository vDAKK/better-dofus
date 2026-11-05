#pragma once

// « Recherche d'objets » (2.4.0): a window of the game's own kind, from the top bar,
// that finds an item by name among the bag and the bank and says how many of it the
// bag, the bank and the havenbag hold.
void InstallItemSearch();
void TickItemSearch();
void RestoreItemSearch();
bool ItemSearchCleanupPending();
void CloseItemSearchOffWorld();
