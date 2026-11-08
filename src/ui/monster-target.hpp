#pragma once

// « Monstres ciblés » (2.15.0): a « Cibler » checkbox in the bestiary's monster card,
// and on the map a violet archimonster badge above every group with a targeted
// monster. The list is saved in the settings (targetMonster=).
void InstallMonsterTarget();
void TickMonsterTarget();
void RestoreMonsterTarget();
bool MonsterTargetCleanupPending();
