#pragma once

// A row in the right-click menu of a boss's group, opening a small native window with
// its achievements and its dungeon's: what is left, with its description, and what is
// done. Everything comes from the game's data and what it already knows of the character.
void InstallBossAchievements();
void TickBossAchievements();
void RestoreBossAchievements();
bool BossAchievementsCleanupPending();
void CloseBossAchievementsOffWorld();
