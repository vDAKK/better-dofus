#pragma once

// A row in the right-click menu of a group whose monsters have achievements, opening a
// small native window with them, and with their dungeon's for a boss: what is left, with
// its description, and what this character has done. Everything comes from the game's
// data and what it already knows of the character.
void InstallBossAchievements();
void TickBossAchievements();
void RestoreBossAchievements();
bool BossAchievementsCleanupPending();
void CloseBossAchievementsOffWorld();
