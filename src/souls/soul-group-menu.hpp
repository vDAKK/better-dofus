#pragma once
#include "core/runtime.hpp"
void InstallSoulGroupMenu();
void TickSoulGroupMenu();
void RestoreSoulGroupMenu();
bool HandleSoulGroupMenuCallback(managed::Obj callback, managed::Obj view);
// Told of each monster group's menu as it is created, out of combat, to add its own row.
// This file owns the only hook on that menu.
void SetSoulGroupMenuExtension(void (*extension)(managed::Obj menu, managed::Obj group));
