#include "weapon_stats.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

// The value WEAPONTEXT lists for a stat is the one at this star, not at star 0.
constexpr float kNeutralStar = 5.0f;
constexpr float kFramesPerSecond = 60.0f;
const char *const kFilledStar = "\xE2\x98\x85";
const char *const kEmptyStar = "\xE2\x98\x86";

// Checked against in-game values: times, spread, energy cost and fire intervals improve by going
// down. A negative a flips a stat of those types (Maximum Energy) so it goes up.
bool LowerIsBetter(int type) {
    switch (type) {
    case 13:
    case 21:
    case 25:
    case 29:
    case 41:
    case 45:
        return true;
    default:
        return false;
    }
}

// Both ROF types store the interval between shots in frames.
bool IsFrameInterval(int type) {
    return type == 25 || type == 29;
}

std::vector<std::string> Split(const std::string &s, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t end = s.find(separator, start);
        parts.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            return parts;
        }
        start = end + 1;
    }
}

// A stat capped below star 5 still shows star 5 when maxed (Slugger NN1's ROF, level 3 of 3, is
// shown as star 5), so its levels are shifted up to end at 5.
int StarOf(const StarValue &v, int level) {
    return level + (v.maxLevel < kNeutralStar ? (int)kNeutralStar - v.maxLevel : 0);
}

float ValueAt(const StarValue &v, int star) {
    const float shift = v.a * (std::pow(star / kNeutralStar, v.b) - 1.0f);
    float value = v.base * (LowerIsBetter(v.type) ? 1.0f - shift : 1.0f + shift);
    if (!v.fractional) {
        value = (float)std::lround(value);
    }
    return value > 0.0f ? value : 0.0f;
}

// Same precision the game uses: one decimal, two significant digits below 1, whole numbers for
// counts, and fire intervals shown as shots per second.
std::string FormatNumber(const StarValue &v, float value) {
    char buf[32];
    if (IsFrameInterval(v.type)) {
        snprintf(buf, sizeof(buf), "%.1f", value > 0.0f ? kFramesPerSecond / value : 0.0f);
    } else if (!v.fractional) {
        snprintf(buf, sizeof(buf), "%.0f", value);
    } else if (std::fabs(value) >= 1.0f) {
        snprintf(buf, sizeof(buf), "%.1f", value);
    } else {
        snprintf(buf, sizeof(buf), "%.2g", value);
    }
    return buf;
}

// The panel fonts have no glyph for the fullwidth tilde the game uses in damage ranges.
std::string Readable(const std::string &s) {
    const std::string fullwidthTilde = "\xEF\xBD\x9E";
    const size_t first = s.find_first_not_of(' ');
    const size_t last = s.find_last_not_of(' ');
    std::string out = first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    for (size_t at = out.find(fullwidthTilde); at != std::string::npos; at = out.find(fullwidthTilde, at + 1)) {
        out.replace(at, fullwidthTilde.size(), "~");
    }
    return out;
}

std::string Fill(const std::string &format, const std::vector<std::string> &numbers) {
    std::string out = format;
    for (int i = (int)numbers.size() - 1; i >= 0; i--) {
        const std::string token = "$" + std::to_string(i);
        for (size_t at = out.find(token); at != std::string::npos; at = out.find(token, at + numbers[i].size())) {
            out.replace(at, token.size(), numbers[i]);
        }
    }
    return Readable(out);
}

int LevelOf(const Weapon &w, const StarValue &v) {
    return v.upgrade >= 0 && v.upgrade < (int)w.upgradeNow.size() ? w.upgradeNow[v.upgrade] : 0;
}

enum class Point { Base, Now, Max };

std::string RenderAt(const Weapon &w, const StatSpec &s, Point point) {
    std::vector<std::string> numbers;
    for (const StarValue &v : s.values) {
        const int level = point == Point::Base ? 0 : (point == Point::Max ? v.maxLevel : LevelOf(w, v));
        numbers.push_back(FormatNumber(v, ValueAt(v, StarOf(v, level))));
    }
    return Fill(s.format, numbers);
}

void AddStarPrefix(TooltipLine &line, const Weapon &w, const StatSpec &s) {
    const StarValue &v = s.values.front();
    const int level = LevelOf(w, v);
    const bool maxed = level >= v.maxLevel;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%d/%d  ", maxed ? kFilledStar : kEmptyStar, StarOf(v, level), StarOf(v, v.maxLevel));
    line.push_back({buf, maxed ? Tone::Max : Tone::Muted});
}

void AddValues(TooltipLine &line, const std::vector<TextRun> &values) {
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0) {
            line.push_back({" / ", Tone::Muted});
        }
        line.push_back(values[i]);
    }
}

TooltipLine StatLine(const Weapon &w, const StatSpec &s, StatView view) {
    TooltipLine line;
    if (s.values.empty()) {
        line.push_back({s.label + ": " + Readable(s.format), Tone::Normal});
        return line;
    }
    const std::string base = RenderAt(w, s, Point::Base);
    const std::string max = RenderAt(w, s, Point::Max);
    if (view == StatView::BaseMax) {
        line.push_back({s.label + ": ", Tone::Normal});
        AddValues(line, {{base, Tone::Normal}, {max, Tone::Max}});
        return line;
    }
    const std::string now = RenderAt(w, s, Point::Now);
    AddStarPrefix(line, w, s);
    line.push_back({s.label + ": ", Tone::Normal});
    if (view == StatView::BaseNowMax) {
        AddValues(line, {{base, Tone::Muted}, {now, Tone::Normal}, {max, Tone::Max}});
    } else if (now == max) {
        AddValues(line, {{now, Tone::Max}});
    } else {
        AddValues(line, {{now, Tone::Normal}, {max, Tone::Max}});
    }
    return line;
}

void AddLegacyLines(std::vector<TooltipLine> &lines, const Weapon &w) {
    size_t start = 0;
    while (start < w.stats.size()) {
        size_t end = w.stats.find(" | ", start);
        if (end == std::string::npos) {
            end = w.stats.size();
        }
        lines.push_back({{Readable(w.stats.substr(start, end - start)), Tone::Normal}});
        start = end + 3;
    }
}

}

std::vector<StatSpec> ParseStatSpecs(const std::string &column) {
    std::vector<StatSpec> specs;
    if (column.empty()) {
        return specs;
    }
    for (const std::string &stat : Split(column, ';')) {
        const std::vector<std::string> fields = Split(stat, '|');
        if (fields.size() < 2) {
            continue;
        }
        StatSpec spec;
        spec.label = fields[0];
        spec.format = fields[1];
        for (size_t i = 2; i < fields.size(); i++) {
            const std::vector<std::string> n = Split(fields[i], ',');
            if (n.size() != 7) {
                continue;
            }
            StarValue v;
            v.base = (float)atof(n[0].c_str());
            v.type = atoi(n[1].c_str());
            v.upgrade = atoi(n[2].c_str());
            v.maxLevel = atoi(n[3].c_str());
            v.a = (float)atof(n[4].c_str());
            v.b = (float)atof(n[5].c_str());
            v.fractional = atoi(n[6].c_str()) != 0;
            spec.values.push_back(v);
        }
        specs.push_back(spec);
    }
    return specs;
}

std::vector<TooltipLine> StatsTooltip(const Weapon &w, StatView view) {
    std::vector<TooltipLine> lines;
    lines.push_back({{w.name, Tone::Title}});
    if (w.statSpecs.empty()) {
        lines.push_back({});
        AddLegacyLines(lines, w);
        return lines;
    }
    const std::string base = std::string(kEmptyStar) + "0";
    switch (view) {
    case StatView::BaseMax:
        lines.push_back({{base + " / ", Tone::Muted}, {"max", Tone::Max}});
        break;
    case StatView::NowMax:
        lines.push_back({{"now / ", Tone::Muted}, {"max", Tone::Max}, {"   hold Ctrl for " + base, Tone::Muted}});
        break;
    case StatView::BaseNowMax:
        lines.push_back({{base + " / now / ", Tone::Muted}, {"max", Tone::Max}});
        break;
    }
    lines.push_back({});
    for (const StatSpec &s : w.statSpecs) {
        lines.push_back(StatLine(w, s, view));
    }
    return lines;
}
