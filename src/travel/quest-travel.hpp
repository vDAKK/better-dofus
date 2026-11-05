#pragma once

// « Trajet depuis les quêtes » (settings.questTravel, 2.4.0): the right-click menu of
// a followed quest's objective offers the mod's trip to its position, when the game's
// data gives it one. Never throws: QUEST_TRAVEL_UNAVAILABLE leaves the menu as it is.
void InstallQuestTravel();
