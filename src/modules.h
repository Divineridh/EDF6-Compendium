#pragma once

#include "edf6_overlay_api.h"

constexpr int kMaxModules = 8;

int ModuleCount();
const Edf6OverlayModule *ModuleAt(int index);
bool AnyModuleWantsDraw();
void DrawModules(float scale);
