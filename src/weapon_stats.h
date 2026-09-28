#pragma once

#include <string>
#include <vector>

#include "compendium.h"

std::vector<StatSpec> ParseStatSpecs(const std::string &column);

enum class StatView { BaseMax, NowMax, BaseNowMax };

enum class Tone { Title, Muted, Normal, Max };

struct TextRun {
    std::string text;
    Tone tone;
};

using TooltipLine = std::vector<TextRun>;

// Tooltip for a weapon: its name, a line saying which columns are shown and one line per stat.
std::vector<TooltipLine> StatsTooltip(const Weapon &w, StatView view);
