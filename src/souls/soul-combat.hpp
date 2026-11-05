#pragma once
#include "core/runtime.hpp"
#include <vector>

namespace soul {
enum class CombatState { Hidden, Pending, Ready, Unsupported, Fighting };
struct CombatSnapshot {
    CombatState state = CombatState::Hidden;
    uint64_t fightIdentity = 0;
    int highestLevel = 0;
};
void InstallCombat();
void ResetCombat();
CombatSnapshot ReadCombat(managed::Obj fight, managed::Obj cached);
// The monster ids of the character's enemies, summons left out, each once; empty out of
// a fight or while its roster is not known. context: what tells one fight from another.
struct EnemyMonsters { std::vector<int> ids; managed::Obj context = nullptr; };
EnemyMonsters ReadEnemyMonsters(managed::Obj fight);
}
