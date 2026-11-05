#pragma once

// « Fiche de recette » (settings.recipeCard, 2.2.0): opening an item's recipe shows
// the game's own pinned item tooltip, the one kept from Alt, with a recipe block,
// instead of the large window of every related recipe. Items without a recipe of
// their own, and any failure, still open the game's window. Every item tooltip also
// gets the average price of its craft. Never throws: RECIPE_CARD_UNAVAILABLE leaves
// the game's recipes as they are.
void InstallRecipeCard();
