#pragma once

constexpr int kClassCount = 4;
constexpr int kSlotsPerClass = 6;

struct Equipment {
    int activeClass = 0;
    int slots[kClassCount][kSlotsPerClass] = {};
};

bool ReadEquipment(Equipment &out);
void InitLoadouts();
