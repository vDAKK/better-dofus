#pragma once

// « Sélection avec Maj » (2.14.0): in the auction house's sell tab, a Shift click on a
// lot, its line or its checkbox, also selects every lot between it and the last one
// clicked without Shift, those scrolled out of view included. The game's own Shift
// click only reached the lines on screen.
void InstallAuctionHouseSelect();
