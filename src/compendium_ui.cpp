#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "compendium.h"
#include "compendium_ui.h"
#include "ui_kit.h"
#include "weapon_stats.h"

using namespace ui;

namespace {

constexpr float kWindowW = 1500.0f;
constexpr float kWindowH = 810.0f;
constexpr float kHeaderH = 58.0f;
constexpr float kStatsH = 76.0f;
constexpr float kSidebarW = 240.0f;
constexpr float kDetailW = 440.0f;
constexpr float kCategoryH = 46.0f;
constexpr float kListHeaderH = 34.0f;
constexpr float kRowH = 38.0f;
constexpr float kLegendH = 38.0f;
constexpr float kDropRowH = 35.0f;
constexpr float kDesignScale = 0.8f;

constexpr uint32_t kFarmCard = 0x10261A;

const char *const kDifficulties[] = {"Normal", "Hard", "Hardest", "Inferno"};
constexpr int kDifficultyCount = 4;
const uint32_t kDifficultyColors[kDifficultyCount] = {0x7FB3FF, 0xE8C46A, 0xFF9A5C, 0xFF6B6B};

enum class Filter { All, Missing, NotMaxed, Wishlist };
enum class View { Class, Farming, Missions, Strats };
const char *const kViewNames[] = {"Farming", "Missions", "Strats"};
constexpr int kViewCount = 3;

struct PanelState {
    View view = View::Class;
    int viewedClass = 0;
    int category = -1;
    Filter filter = Filter::All;
    char search[64] = "";
    bool focusSearch = false;
    int selected = -1;
    int difficulty = -1;
    bool scrollToSelected = false;
    int farmClass = -1;
    bool farmWishOnly = false;
    int missionDifficulty = 3;
    std::string mission;
    int strat = 0;
    unsigned version = 0;
};

PanelState g;

struct Counts {
    int owned = 0;
    int total = 0;
    int maxed = 0;
    int wished = 0;
    int notMaxed = 0;
    int farmableMissing = 0;
};

struct Category {
    int id;
    std::string name;
    Counts counts;
};

struct Row {
    bool header;
    const Category *category;
    Weapon *weapon;
};

Counts CountOf(const std::vector<Weapon> &weapons, int category) {
    Counts c;
    for (const Weapon &w : weapons) {
        if (category != -1 && w.category != category) {
            continue;
        }
        c.total++;
        c.owned += w.owned ? 1 : 0;
        c.maxed += w.starred ? 1 : 0;
        c.wished += w.wish ? 1 : 0;
        c.notMaxed += w.owned && !w.starred ? 1 : 0;
        c.farmableMissing += !w.owned && w.farmable ? 1 : 0;
    }
    return c;
}

std::vector<Category> CategoriesOf(const ClassData &cls) {
    std::vector<Category> out;
    for (const Weapon &w : cls.weapons) {
        bool seen = false;
        for (const Category &c : out) {
            seen = seen || c.id == w.category;
        }
        if (!seen) {
            out.push_back(Category{w.category, w.categoryName, Counts()});
        }
    }
    for (Category &c : out) {
        c.counts = CountOf(cls.weapons, c.id);
    }
    return out;
}

bool ContainsNoCase(const std::string &text, const char *needle) {
    const size_t n = strlen(needle);
    if (n == 0) {
        return true;
    }
    for (size_t i = 0; i + n <= text.size(); i++) {
        size_t j = 0;
        while (j < n && tolower((unsigned char)text[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == n) {
            return true;
        }
    }
    return false;
}

bool Passes(const Weapon &w) {
    switch (g.filter) {
    case Filter::Missing:
        if (w.owned) return false;
        break;
    case Filter::NotMaxed:
        if (!w.owned || w.starred) return false;
        break;
    case Filter::Wishlist:
        if (!w.wish) return false;
        break;
    case Filter::All:
        break;
    }
    return ContainsNoCase(w.name, g.search) || ContainsNoCase(w.categoryName, g.search);
}

std::vector<Row> BuildRows(ClassData &cls, const std::vector<Category> &categories) {
    std::vector<Row> rows;
    for (const Category &c : categories) {
        if (g.category != -1 && g.category != c.id) {
            continue;
        }
        bool headerAdded = false;
        for (Weapon &w : cls.weapons) {
            if (w.category != c.id || !Passes(w)) {
                continue;
            }
            if (!headerAdded) {
                rows.push_back(Row{true, &c, nullptr});
                headerAdded = true;
            }
            rows.push_back(Row{false, &c, &w});
        }
    }
    return rows;
}

void UpgradeProgress(const Weapon &w, int &now, int &max) {
    now = 0;
    max = 0;
    for (size_t i = 0; i < w.upgradeMax.size(); i++) {
        max += w.upgradeMax[i];
        const int v = i < w.upgradeNow.size() ? w.upgradeNow[i] : 0;
        now += v < w.upgradeMax[i] ? v : w.upgradeMax[i];
    }
}

std::vector<const Drop *> DropsFor(const Weapon &w) {
    std::vector<const Drop *> out;
    for (const Drop &d : GetDrops()) {
        if (w.tier <= d.tier && d.lo <= w.level && w.level <= d.hi) {
            out.push_back(&d);
        }
    }
    std::sort(out.begin(), out.end(), [](const Drop *a, const Drop *b) { return a->chance > b->chance; });
    return out;
}

int DifficultyIndex(const std::string &name) {
    for (int i = 0; i < kDifficultyCount; i++) {
        if (name == kDifficulties[i]) {
            return i;
        }
    }
    return -1;
}

uint32_t DifficultyColor(const std::string &name) {
    const int i = DifficultyIndex(name);
    return i < 0 ? kSoft : kDifficultyColors[i];
}

void StatusSquare(ImDrawList *dl, ImVec2 p, const Weapon &w) {
    const ImVec2 q(p.x + D(9.0f), p.y + D(9.0f));
    if (!w.owned) {
        dl->AddRect(p, q, Rgb(kFaint), 0.0f, D(1.0f));
    } else {
        dl->AddRectFilled(p, q, Rgb(w.starred ? kAmber : kGreen));
    }
}

void ProgressLine(ImDrawList *dl, ImVec2 p, float width, float fraction, uint32_t col, float thickness) {
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + thickness), Rgb(kKeyLine));
    if (fraction > 0.0f) {
        dl->AddRectFilled(p, ImVec2(p.x + width * (fraction < 1.0f ? fraction : 1.0f), p.y + thickness), Rgb(col));
    }
}

void SelectWeapon(int index) {
    g.selected = index;
    g.difficulty = -1;
}

void ToggleWish(Weapon &w) {
    w.wish = !w.wish;
    SaveWishlist();
    g.version++;
}

int CurrentTab(int classCount) {
    return g.view == View::Class ? g.viewedClass : classCount + (int)g.view - 1;
}

void SwitchTab(int tab, int classCount) {
    const int total = classCount + kViewCount;
    tab = (tab + total) % total;
    if (tab >= classCount) {
        g.view = (View)(tab - classCount + 1);
        return;
    }
    if (g.view != View::Class || tab != g.viewedClass) {
        g.category = -1;
        g.selected = -1;
    }
    g.view = View::Class;
    g.viewedClass = tab;
    g.scrollToSelected = true;
}

ImVec4 ToneColor(Tone tone) {
    switch (tone) {
    case Tone::Title:
        return ImGui::ColorConvertU32ToFloat4(Rgb(kText));
    case Tone::Muted:
        return ImGui::ColorConvertU32ToFloat4(Rgb(kMuted));
    case Tone::Max:
        return ImGui::ColorConvertU32ToFloat4(Rgb(kMaxed));
    default:
        return ImGui::ColorConvertU32ToFloat4(Rgb(kSoft));
    }
}

// Ctrl is read straight from the keyboard: the game doesn't always pass key messages on, so
// imgui's own KeyCtrl stays false.
void ShowStatsTooltip(const Weapon &w, const char *footer = nullptr) {
    StatView view = StatView::BaseMax;
    if (w.owned) {
        view = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ? StatView::BaseNowMax : StatView::NowMax;
    }
    ImGui::BeginTooltip();
    for (const TooltipLine &line : StatsTooltip(w, view)) {
        if (line.empty()) {
            ImGui::TextUnformatted("");
            continue;
        }
        for (size_t i = 0; i < line.size(); i++) {
            if (i > 0) {
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(line[i].tone));
            ImGui::TextUnformatted(line[i].text.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (footer) {
        ImGui::TextUnformatted("");
        ImGui::TextUnformatted(footer);
    }
    ImGui::EndTooltip();
}

std::string SaveAge() {
    const unsigned long long at = MomentoLecturaSave();
    if (!at || MarcadasEnSave() == 0) {
        return std::string();
    }
    const unsigned long long seconds = (GetTickCount64() - at) / 1000;
    char buf[48];
    if (seconds < 60) {
        snprintf(buf, sizeof(buf), "Save read just now");
    } else if (seconds < 3600) {
        snprintf(buf, sizeof(buf), "Save read %llu min ago", seconds / 60);
    } else {
        snprintf(buf, sizeof(buf), "Save read %llu h ago", seconds / 3600);
    }
    return buf;
}

bool SmallButton(ImDrawList *dl, const char *id, ImVec2 p, const char *label) {
    const ImVec2 size = Measure(g_fontSemi, 14.0f, label);
    const ImVec2 box(size.x + D(22.0f), D(28.0f));
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, box);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 q(p.x + box.x, p.y + box.y);
    if (hovered) {
        dl->AddRectFilled(p, q, Rgb(kSelected));
    }
    dl->AddRect(p, q, Rgb(kKeyLine), 0.0f, D(1.0f));
    PaintText(dl, g_fontSemi, 14.0f, ImVec2(p.x + D(11.0f), p.y + (box.y - size.y) * 0.5f), kText, label);
    return clicked;
}

void DrawHeader(ImDrawList *dl, ImVec2 o, float w, Catalog &cat, bool &open) {
    const float h = D(kHeaderH);
    dl->AddRectFilled(ImVec2(o.x + D(20.0f), o.y + D(25.0f)), ImVec2(o.x + D(28.0f), o.y + D(33.0f)), Rgb(kGreen));
    SpacedText(dl, g_fontBold, 16.0f, ImVec2(o.x + D(38.0f), o.y + D(18.0f)), kText, "COMPENDIUM", 2.5f);
    dl->AddLine(ImVec2(o.x + D(200.0f), o.y), ImVec2(o.x + D(200.0f), o.y + h), Rgb(kLine), D(1.0f));

    const int classCount = (int)cat.classes.size();
    float x = o.x + D(214.0f);
    x += KeyHint(dl, ImVec2(x, o.y + D(18.0f)), "Q", kMuted, kKeyLine) + D(10.0f);
    for (int c = 0; c < classCount; c++) {
        const Counts counts = CountOf(cat.classes[c].weapons, -1);
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", counts.total ? counts.owned * 100 / counts.total : 0);
        const char *name = cat.classes[c].name.c_str();
        const ImVec2 nameSize = Measure(g_fontLabel, 16.0f, name);
        const ImVec2 pctSize = Measure(g_fontLabel, 12.0f, pct);
        const float tabW = D(16.0f) + nameSize.x + D(7.0f) + pctSize.x + D(16.0f);

        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        ImGui::PushID(c);
        if (ImGui::InvisibleButton("tab", ImVec2(tabW, h))) {
            SwitchTab(c, classCount);
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const bool viewed = g.view == View::Class && c == g.viewedClass;
        const float textY = o.y + (h - nameSize.y) * 0.5f;
        PaintText(dl, g_fontLabel, 16.0f, ImVec2(x + D(16.0f), textY), viewed || hovered ? kText : kMuted, name);
        PaintText(dl, g_fontLabel, 12.0f,
                  ImVec2(x + D(16.0f) + nameSize.x + D(7.0f), textY + nameSize.y - pctSize.y - D(2.0f)), kFaint, pct);
        if (viewed) {
            dl->AddRectFilled(ImVec2(x, o.y + h - D(2.0f)), ImVec2(x + tabW, o.y + h), Rgb(kGreen));
        }
        x += tabW;
    }
    x += D(8.0f);
    dl->AddLine(ImVec2(x, o.y + D(18.0f)), ImVec2(x, o.y + h - D(18.0f)), Rgb(kLine), D(1.0f));
    x += D(8.0f);
    for (int v = 0; v < kViewCount; v++) {
        const ImVec2 nameSize = Measure(g_fontLabel, 16.0f, kViewNames[v]);
        const float tabW = D(16.0f) + nameSize.x + D(16.0f);
        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        ImGui::PushID(100 + v);
        if (ImGui::InvisibleButton("view", ImVec2(tabW, h))) {
            SwitchTab(classCount + v, classCount);
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool viewed = (int)g.view == v + 1;
        PaintText(dl, g_fontLabel, 16.0f, ImVec2(x + D(16.0f), o.y + (h - nameSize.y) * 0.5f),
                  viewed || hovered ? kText : kMuted, kViewNames[v]);
        if (viewed) {
            dl->AddRectFilled(ImVec2(x, o.y + h - D(2.0f)), ImVec2(x + tabW, o.y + h), Rgb(kGreen));
        }
        x += tabW;
    }
    KeyHint(dl, ImVec2(x + D(10.0f), o.y + D(18.0f)), "E", kMuted, kKeyLine);

    const std::string key = KeyName(TeclaToggle());
    const ImVec2 closeSize = Measure(g_fontLabel, 13.0f, "close");
    const float keyW = Measure(g_fontLabel, 12.0f, key.c_str()).x + D(12.0f);
    const float closeX = o.x + w - D(20.0f) - closeSize.x;
    const float keyX = closeX - D(8.0f) - keyW;
    ImGui::SetCursorScreenPos(ImVec2(keyX, o.y + D(14.0f)));
    if (ImGui::InvisibleButton("close", ImVec2(o.x + w - keyX, D(30.0f)))) {
        open = false;
    }
    const bool closeHovered = ImGui::IsItemHovered();
    KeyHint(dl, ImVec2(keyX, o.y + D(18.0f)), key.c_str(), kMuted, kKeyLine);
    PaintText(dl, g_fontLabel, 13.0f, ImVec2(closeX, o.y + (h - closeSize.y) * 0.5f), closeHovered ? kText : kMuted,
              "close");
    dl->AddLine(ImVec2(keyX - D(16.0f), o.y + D(16.0f)), ImVec2(keyX - D(16.0f), o.y + h - D(16.0f)), Rgb(kLine),
                D(1.0f));

    const float reloadW = Measure(g_fontSemi, 14.0f, "Reload").x + D(22.0f);
    const float reloadX = keyX - D(30.0f) - reloadW;
    if (SmallButton(dl, "reload", ImVec2(reloadX, o.y + D(15.0f)), "Reload")) {
        LeerObtenidasDelSave();
    }
    const std::string age = SaveAge();
    const char *status = age.empty() ? "Save not read" : age.c_str();
    const ImVec2 statusSize = Measure(nullptr, 13.0f, status);
    const float statusX = reloadX - D(14.0f) - statusSize.x;
    const float statusY = o.y + (h - statusSize.y) * 0.5f;
    dl->AddRectFilled(ImVec2(statusX - D(14.0f), o.y + h * 0.5f - D(3.0f)),
                      ImVec2(statusX - D(8.0f), o.y + h * 0.5f + D(3.0f)), Rgb(age.empty() ? kAmber : kGreen));
    PaintText(dl, nullptr, 13.0f, ImVec2(statusX, statusY), age.empty() ? kAmber : kMuted, status);
    if (ImGui::IsMouseHoveringRect(ImVec2(statusX, statusY), ImVec2(statusX + statusSize.x, statusY + statusSize.y))) {
        ImGui::SetTooltip("%s", age.empty() ? "Marks come from weapons.tsv, not from your save. See Compendium.log."
                                            : SavePathUsado());
    }

    const std::vector<std::string> &fresh = NuevasDesdeUltimaMision();
    if (!fresh.empty()) {
        char label[32];
        snprintf(label, sizeof(label), "+%d new", (int)fresh.size());
        const ImVec2 ls = Measure(g_fontSemi, 13.0f, label);
        const ImVec2 a(statusX - D(30.0f) - ls.x - D(16.0f), o.y + D(16.0f));
        const ImVec2 b(a.x + ls.x + D(16.0f), o.y + h - D(16.0f));
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("fresh", ImVec2(b.x - a.x, b.y - a.y))) {
            DescartarNuevas();
        } else {
            dl->AddRectFilled(a, b, Rgb(kFarmCard));
            dl->AddRect(a, b, Rgb(kGreen), 0.0f, D(1.0f));
            PaintText(dl, g_fontSemi, 13.0f, ImVec2(a.x + D(8.0f), a.y + (b.y - a.y - ls.y) * 0.5f), kGreen, label);
            if (ImGui::IsItemHovered()) {
                std::string list = "New since your last mission:\n";
                for (const std::string &name : fresh) {
                    list += "\n" + name;
                }
                list += "\n\nClick to dismiss.";
                ImGui::SetTooltip("%s", list.c_str());
            }
        }
    }

    dl->AddLine(ImVec2(o.x, o.y + h), ImVec2(o.x + w, o.y + h), Rgb(kLine), D(1.0f));
}

void StatBlock(ImDrawList *dl, float x, float top, const char *label, int value, uint32_t col) {
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(x, top + D(16.0f)), kFaint, label, 1.5f);
    char v[16];
    snprintf(v, sizeof(v), "%d", value);
    PaintText(dl, g_fontBold, 20.0f, ImVec2(x, top + D(33.0f)), col, v);
}

struct SegmentItem {
    const char *label;
    int count;
};

float SegmentWidth(const SegmentItem &item) {
    char n[16];
    snprintf(n, sizeof(n), "%d", item.count);
    const float countW = item.count >= 0 ? D(7.0f) + Measure(g_fontLabel, 12.0f, n).x : 0.0f;
    return D(14.0f) + Measure(g_fontSemi, 15.0f, item.label).x + countW + D(14.0f);
}

bool Segment(ImDrawList *dl, float &x, float y, float h, const char *label, int count, bool selected, int id) {
    char n[16];
    snprintf(n, sizeof(n), "%d", count);
    const ImVec2 labelSize = Measure(g_fontSemi, 15.0f, label);
    const ImVec2 countSize = Measure(g_fontLabel, 12.0f, n);
    const float w = SegmentWidth(SegmentItem{label, count});
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("segment", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (selected || hovered) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Rgb(kSelected, selected ? 1.0f : 0.6f));
    }
    const float ty = y + (h - labelSize.y) * 0.5f;
    PaintText(dl, g_fontSemi, 15.0f, ImVec2(x + D(14.0f), ty), selected ? kText : kSoft, label);
    if (count >= 0) {
        PaintText(dl, g_fontLabel, 12.0f,
                  ImVec2(x + D(14.0f) + labelSize.x + D(7.0f), ty + labelSize.y - countSize.y - D(2.0f)), kFaint, n);
    }
    x += w;
    return clicked;
}

int SegmentGroup(ImDrawList *dl, float rightX, float y, const SegmentItem *items, int n, int selected, int idBase,
                 float &left) {
    const float h = D(42.0f);
    float groupW = 0.0f;
    for (int i = 0; i < n; i++) {
        groupW += SegmentWidth(items[i]);
    }
    float x = rightX - groupW;
    left = x;
    int clicked = -1;
    for (int i = 0; i < n; i++) {
        if (Segment(dl, x, y, h, items[i].label, items[i].count, i == selected, idBase + i)) {
            clicked = i;
        }
    }
    dl->AddRect(ImVec2(left, y), ImVec2(left + groupW, y + h), Rgb(kKeyLine), 0.0f, D(1.0f));
    return clicked;
}

void DrawStats(ImDrawList *dl, ImVec2 o, float w, const Counts &counts) {
    const float top = o.y + D(kHeaderH);
    char big[16];
    snprintf(big, sizeof(big), "%d", counts.owned);
    const ImVec2 bigSize = Measure(g_fontBold, 30.0f, big);
    PaintText(dl, g_fontBold, 30.0f, ImVec2(o.x + D(20.0f), top + D(12.0f)), kText, big);
    char rest[32];
    snprintf(rest, sizeof(rest), " / %d collected", counts.total);
    const ImVec2 restSize = Measure(nullptr, 16.0f, rest);
    PaintText(dl, nullptr, 16.0f, ImVec2(o.x + D(20.0f) + bigSize.x, top + D(12.0f) + bigSize.y - restSize.y - D(3.0f)),
              kMuted, rest);
    ProgressLine(dl, ImVec2(o.x + D(20.0f), top + D(58.0f)), D(300.0f),
                 counts.total ? (float)counts.owned / counts.total : 0.0f, kGreen, D(3.0f));

    StatBlock(dl, o.x + D(348.0f), top, "MISSING", counts.total - counts.owned, kText);
    StatBlock(dl, o.x + D(430.0f), top, "FULLY UPGRADED", counts.maxed, kAmber);
    StatBlock(dl, o.x + D(572.0f), top, "WISHLIST", counts.wished, kPink);

    const float searchX = o.x + D(778.0f);
    const float searchY = top + D(14.0f);
    const float searchW = D(306.0f);
    const float searchH = D(42.0f);
    dl->AddRect(ImVec2(searchX, searchY), ImVec2(searchX + searchW, searchY + searchH),
                Rgb(ImGui::GetIO().WantTextInput ? kGreen : kKeyLine), 0.0f, D(1.0f));
    const float hintW = KeyHint(dl, ImVec2(searchX + D(10.0f), searchY + D(10.0f)), "/", kMuted, kKeyLine);
    ImGui::SetCursorScreenPos(ImVec2(searchX + D(10.0f) + hintW + D(6.0f), searchY + D(5.0f)));
    ImGui::SetNextItemWidth(searchW - hintW - D(26.0f));
    ImGui::PushFont(nullptr, FontBase(15.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgb(kText));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Rgb(kFaint));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(D(4.0f), D(6.0f)));
    if (g.focusSearch) {
        ImGui::SetKeyboardFocusHere();
        g.focusSearch = false;
    }
    if (ImGui::InputTextWithHint("##search", "Search weapons", g.search, sizeof(g.search))) {
        g.scrollToSelected = true;
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();

    const SegmentItem segments[] = {
        {"All", counts.total},
        {"Missing", counts.total - counts.owned},
        {"Not maxed", counts.notMaxed},
        {"Wishlist", counts.wished},
    };
    float left = 0.0f;
    const int clicked = SegmentGroup(dl, o.x + w - D(20.0f), searchY, segments, 4, (int)g.filter, 0, left);
    if (clicked >= 0) {
        g.filter = (Filter)clicked;
        g.scrollToSelected = true;
    }

    const float missingX = o.x + D(348.0f);
    if (ImGui::IsMouseHoveringRect(ImVec2(missingX, top), ImVec2(missingX + D(70.0f), top + D(kStatsH)))) {
        const int missing = counts.total - counts.owned;
        ImGui::SetTooltip("%d of the %d missing can drop from crates.\nThe other %d are mission or DLC rewards.",
                          counts.farmableMissing, missing, missing - counts.farmableMissing);
    }

    dl->AddLine(ImVec2(o.x, top + D(kStatsH)), ImVec2(o.x + w, top + D(kStatsH)), Rgb(kLine), D(1.0f));
}

void CategoryRow(ImDrawList *dl, ImVec2 p, float w, const char *name, const Counts &c, bool selected, bool bold) {
    if (selected) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + D(kCategoryH)), Rgb(kSelected));
    }
    PaintText(dl, bold ? g_fontBold : g_fontLabel, 16.0f, ImVec2(p.x + D(20.0f), p.y + D(11.0f)), kText, name);
    char n[16];
    snprintf(n, sizeof(n), "%d/%d", c.owned, c.total);
    const ImVec2 ns = Measure(g_fontMono, 13.0f, n);
    PaintText(dl, g_fontMono, 13.0f, ImVec2(p.x + w - D(20.0f) - ns.x, p.y + D(13.0f)),
              c.owned == c.total ? kGreen : kFaint, n);
    ProgressLine(dl, ImVec2(p.x + D(20.0f), p.y + D(35.0f)), w - D(40.0f), c.total ? (float)c.owned / c.total : 0.0f,
                 kGreen, D(2.0f));
}

void DrawSidebar(ImVec2 o, float bodyTop, float bodyBottom, const std::vector<Category> &categories,
                 const Counts &classCounts) {
    ImGui::SetCursorScreenPos(ImVec2(o.x, bodyTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("categories", ImVec2(D(kSidebarW) - D(1.0f), bodyBottom - bodyTop), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList *cdl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;

    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("all", ImVec2(w, D(kCategoryH)))) {
        g.category = -1;
        g.scrollToSelected = true;
    }
    CategoryRow(cdl, p, w, "All", classCounts, g.category == -1, true);
    for (const Category &c : categories) {
        p = ImGui::GetCursorScreenPos();
        ImGui::PushID(c.id);
        if (ImGui::InvisibleButton("category", ImVec2(w, D(kCategoryH)))) {
            g.category = c.id;
            g.scrollToSelected = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered && g.category != c.id) {
            cdl->AddRectFilled(p, ImVec2(p.x + w, p.y + D(kCategoryH)), Rgb(kSelected, 0.5f));
        }
        CategoryRow(cdl, p, w, c.name.c_str(), c.counts, g.category == c.id, false);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

struct ListColumns {
    float name;
    float lvRight;
    float bar;
    float heart;
    float nameWidth;
};

ListColumns ColumnsFor(float x, float rowW) {
    ListColumns c;
    c.name = x + D(52.0f);
    c.lvRight = x + rowW - D(190.0f);
    c.bar = x + rowW - D(170.0f);
    c.heart = x + rowW - D(26.0f);
    c.nameWidth = c.lvRight - c.name - D(60.0f);
    return c;
}

void DrawListRow(ImDrawList *ldl, ImVec2 p, float rowW, Row &r) {
    const float rowH = D(kRowH);
    const ListColumns col = ColumnsFor(p.x, rowW);
    if (r.header) {
        const std::string label = Upper(r.category->name.c_str());
        SpacedText(ldl, g_fontLabel, 12.0f, ImVec2(p.x + D(20.0f), p.y + D(14.0f)), kMuted, label.c_str(), 1.5f);
        char n[16];
        snprintf(n, sizeof(n), "%d/%d", r.category->counts.owned, r.category->counts.total);
        const ImVec2 ns = Measure(g_fontMono, 13.0f, n);
        PaintText(ldl, g_fontMono, 13.0f, ImVec2(p.x + rowW - D(20.0f) - ns.x, p.y + D(14.0f)), kFaint, n);
        return;
    }
    const Weapon &weapon = *r.weapon;
    StatusSquare(ldl, ImVec2(p.x + D(20.0f), p.y + (rowH - D(9.0f)) * 0.5f), weapon);
    const float ty = p.y + (rowH - Measure(g_fontSemi, 16.0f, "Ag").y) * 0.5f;
    FittedText(ldl, g_fontSemi, 16.0f, ImVec2(col.name, ty), weapon.owned ? kText : kMuted, weapon.name, col.nameWidth);

    char lv[8];
    snprintf(lv, sizeof(lv), "%d", weapon.level);
    const ImVec2 lvSize = Measure(g_fontMono, 14.0f, lv);
    PaintText(ldl, g_fontMono, 14.0f, ImVec2(col.lvRight - lvSize.x, p.y + (rowH - lvSize.y) * 0.5f),
              weapon.owned ? kSoft : kFaint, lv);

    int now = 0, max = 0;
    UpgradeProgress(weapon, now, max);
    const bool hasProgress = weapon.owned && max > 0;
    ProgressLine(ldl, ImVec2(col.bar, p.y + rowH * 0.5f), D(72.0f), hasProgress ? (float)now / max : 0.0f,
                 weapon.starred ? kAmber : kSoft, D(2.0f));
    char up[16];
    if (hasProgress) {
        snprintf(up, sizeof(up), "%d/%d", now, max);
    } else {
        snprintf(up, sizeof(up), "-");
    }
    const ImVec2 upSize = Measure(g_fontMono, 13.0f, up);
    PaintText(ldl, g_fontMono, 13.0f, ImVec2(col.bar + D(84.0f), p.y + (rowH - upSize.y) * 0.5f),
              weapon.starred ? kAmber : kFaint, up);
}

void DrawList(ImDrawList *dl, float x, float w, float bodyTop, float bodyBottom, std::vector<Row> &rows) {
    const float rowWidth = w - ImGui::GetStyle().ScrollbarSize;
    const ListColumns header = ColumnsFor(x, rowWidth);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(header.name, bodyTop + D(11.0f)), kFaint, "ITEM", 1.5f);
    const float lvW = Measure(g_fontLabel, 11.0f, "LV").x + D(1.5f) * 2;
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(header.lvRight - lvW, bodyTop + D(11.0f)), kFaint, "LV", 1.5f);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(header.bar, bodyTop + D(11.0f)), kFaint, "UPGRADES", 1.5f);
    dl->AddLine(ImVec2(x, bodyTop + D(kListHeaderH)), ImVec2(x + w, bodyTop + D(kListHeaderH)), Rgb(kLine), D(1.0f));

    const float listTop = bodyTop + D(kListHeaderH) + D(1.0f);
    const float listBottom = bodyBottom - D(kLegendH);
    ImGui::SetCursorScreenPos(ImVec2(x, listTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("items", ImVec2(w, listBottom - listTop), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList *ldl = ImGui::GetWindowDrawList();
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float rowH = D(kRowH);

    if (rows.empty()) {
        const char *empty = "Nothing matches this filter.";
        const ImVec2 es = Measure(nullptr, 15.0f, empty);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(rowW, rowH * 3));
        PaintText(ldl, nullptr, 15.0f, ImVec2(p.x + (rowW - es.x) * 0.5f, p.y + rowH), kMuted, empty);
    }

    ImGuiListClipper clipper;
    clipper.Begin((int)rows.size(), rowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            Row &r = rows[i];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("row", ImVec2(rowW, rowH));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (!r.header) {
                Weapon &weapon = *r.weapon;
                const ListColumns col = ColumnsFor(p.x, rowW);
                const bool onHeart = ImGui::GetIO().MousePos.x >= col.heart - D(18.0f);
                const bool isSelected = weapon.index == g.selected;
                if (isSelected || hovered) {
                    ldl->AddRectFilled(p, ImVec2(p.x + rowW, p.y + rowH), Rgb(kSelected, isSelected ? 1.0f : 0.5f));
                }
                if (clicked) {
                    if (onHeart) {
                        ToggleWish(weapon);
                    } else {
                        SelectWeapon(weapon.index);
                    }
                }
                Heart(ldl, ImVec2(col.heart, p.y + rowH * 0.5f), D(16.0f),
                      weapon.wish ? kPink : (hovered && onHeart ? kMuted : kKeyLine));
                if (!onHeart && !weapon.stats.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ShowStatsTooltip(weapon);
                }
            }
            DrawListRow(ldl, p, rowW, r);
        }
    }
    clipper.End();

    if (g.scrollToSelected) {
        int target = -1;
        for (int i = 0; i < (int)rows.size(); i++) {
            if (!rows[i].header && rows[i].weapon->index == g.selected) {
                target = i;
            }
        }
        if (target < 0) {
            ImGui::SetScrollY(0.0f);
        } else {
            const float y = target * rowH;
            const float view = ImGui::GetWindowHeight();
            if (y < ImGui::GetScrollY()) {
                ImGui::SetScrollY(std::max(0.0f, y - rowH));
            } else if (y + rowH > ImGui::GetScrollY() + view) {
                ImGui::SetScrollY(y + rowH * 2 - view);
            }
        }
        g.scrollToSelected = false;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    const float ly = listBottom + (D(kLegendH) - Measure(nullptr, 13.0f, "Ag").y) * 0.5f;
    dl->AddLine(ImVec2(x, listBottom), ImVec2(x + w, listBottom), Rgb(kLine), D(1.0f));
    float lx = x + D(20.0f);
    const float sq = D(9.0f);
    const float sy = listBottom + (D(kLegendH) - sq) * 0.5f;
    const struct {
        const char *label;
        uint32_t color;
        bool filled;
    } legend[] = {{"Owned", kGreen, true}, {"Fully upgraded", kAmber, true}, {"Missing", kFaint, false}};
    for (const auto &item : legend) {
        if (item.filled) {
            dl->AddRectFilled(ImVec2(lx, sy), ImVec2(lx + sq, sy + sq), Rgb(item.color));
        } else {
            dl->AddRect(ImVec2(lx, sy), ImVec2(lx + sq, sy + sq), Rgb(item.color), 0.0f, D(1.0f));
        }
        PaintText(dl, nullptr, 13.0f, ImVec2(lx + sq + D(7.0f), ly), kMuted, item.label);
        lx += sq + D(7.0f) + Measure(nullptr, 13.0f, item.label).x + D(18.0f);
    }
    const char *hint = "\xE2\x86\x91\xE2\x86\x93 browse \xC2\xB7 W wishlist \xC2\xB7 / search";
    const ImVec2 hs = Measure(nullptr, 13.0f, hint);
    PaintText(dl, nullptr, 13.0f, ImVec2(x + w - D(20.0f) - hs.x, ly), kFaint, hint);
}

void DrawDetail(ImDrawList *dl, float x, float w, float bodyTop, float bodyBottom, const ClassData &cls) {
    Weapon *weapon = g.selected >= 0 ? ArmaPorIndice(g.selected) : nullptr;
    if (!weapon) {
        const char *msg = "Pick a weapon to see how to get it.";
        const ImVec2 ms = Measure(nullptr, 15.0f, msg);
        PaintText(dl, nullptr, 15.0f, ImVec2(x + (w - ms.x) * 0.5f, bodyTop + (bodyBottom - bodyTop) * 0.5f), kMuted, msg);
        return;
    }
    const float left = x + D(24.0f);
    const float right = x + w - D(24.0f);
    float y = bodyTop + D(22.0f);

    char crumb[160];
    snprintf(crumb, sizeof(crumb), "%s \xC2\xB7 %s \xC2\xB7 LV%d", Upper(cls.name.c_str()).c_str(),
             Upper(weapon->categoryName.c_str()).c_str(), weapon->level);
    SpacedText(dl, g_fontLabel, 12.0f, ImVec2(left, y), kFaint, crumb, 1.5f);
    y += D(22.0f);
    FittedText(dl, g_fontBold, 28.0f, ImVec2(left, y), kText, weapon->name, right - left);
    const ImVec2 titleSize = Measure(g_fontBold, 28.0f, weapon->name.c_str());
    if (!weapon->stats.empty() &&
        ImGui::IsMouseHoveringRect(ImVec2(left, y), ImVec2(left + std::min(titleSize.x, right - left), y + titleSize.y))) {
        ShowStatsTooltip(*weapon);
    }
    y += D(46.0f);

    const char *badge = weapon->owned ? "Owned" : "Missing";
    const ImVec2 bs = Measure(g_fontSemi, 14.0f, badge);
    const ImVec2 b0(left, y);
    const ImVec2 b1(left + bs.x + D(20.0f), y + D(28.0f));
    if (weapon->owned) {
        dl->AddRectFilled(b0, b1, Rgb(weapon->starred ? kAmber : kGreen));
        PaintText(dl, g_fontSemi, 14.0f, ImVec2(b0.x + D(10.0f), y + (D(28.0f) - bs.y) * 0.5f), kOnGreen, badge);
    } else {
        dl->AddRect(b0, b1, Rgb(kFaint), 0.0f, D(1.0f));
        PaintText(dl, g_fontSemi, 14.0f, ImVec2(b0.x + D(10.0f), y + (D(28.0f) - bs.y) * 0.5f), kMuted, badge);
    }

    const char *wishLabel = weapon->wish ? "On wishlist" : "Add to wishlist";
    const ImVec2 ws = Measure(g_fontSemi, 14.0f, wishLabel);
    const float keyW = Measure(g_fontLabel, 12.0f, "W").x + D(12.0f);
    const float wishX = b1.x + D(10.0f);
    const float wishW = D(34.0f) + ws.x + D(10.0f) + keyW + D(10.0f);
    ImGui::SetCursorScreenPos(ImVec2(wishX, y));
    if (ImGui::InvisibleButton("wish", ImVec2(wishW, D(28.0f)))) {
        ToggleWish(*weapon);
    }
    const bool wishHovered = ImGui::IsItemHovered();
    if (wishHovered) {
        dl->AddRectFilled(ImVec2(wishX, y), ImVec2(wishX + wishW, y + D(28.0f)), Rgb(kSelected));
    }
    dl->AddRect(ImVec2(wishX, y), ImVec2(wishX + wishW, y + D(28.0f)), Rgb(weapon->wish ? kPink : kKeyLine), 0.0f,
                D(1.0f));
    Heart(dl, ImVec2(wishX + D(17.0f), y + D(14.0f)), D(14.0f), weapon->wish ? kPink : kText);
    PaintText(dl, g_fontSemi, 14.0f, ImVec2(wishX + D(30.0f), y + (D(28.0f) - ws.y) * 0.5f),
              weapon->wish ? kPink : kText, wishLabel);
    KeyHint(dl, ImVec2(wishX + D(30.0f) + ws.x + D(10.0f), y + D(3.0f)), "W", kMuted, kKeyLine);
    y += D(44.0f);

    int now = 0, max = 0;
    UpgradeProgress(*weapon, now, max);
    PaintText(dl, nullptr, 15.0f, ImVec2(left, y), kMuted, "Upgrades");
    char up[32];
    if (weapon->owned && max > 0) {
        snprintf(up, sizeof(up), "%d / %d", now, max);
    } else {
        snprintf(up, sizeof(up), "- / %d", max);
    }
    const ImVec2 us = Measure(g_fontMono, 15.0f, up);
    PaintText(dl, g_fontMono, 15.0f, ImVec2(right - us.x, y + D(1.0f)), weapon->starred ? kAmber : kText, up);
    y += D(26.0f);
    ProgressLine(dl, ImVec2(left, y), right - left, weapon->owned && max ? (float)now / max : 0.0f,
                 weapon->starred ? kAmber : kGreen, D(3.0f));
    y += D(24.0f);
    dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), Rgb(kLine), D(1.0f));
    y += D(22.0f);

    SpacedText(dl, g_fontLabel, 12.0f, ImVec2(left, y), kFaint, "HOW TO GET", 1.5f);
    y += D(26.0f);

    const std::vector<const Drop *> drops = DropsFor(*weapon);
    const ImVec2 cardA(left, y);
    const ImVec2 cardB(right, y + D(84.0f));
    dl->AddRectFilled(cardA, cardB, Rgb(drops.empty() ? kSelected : kFarmCard));
    if (drops.empty()) {
        SpacedText(dl, g_fontLabel, 11.0f, ImVec2(left + D(14.0f), y + D(14.0f)), kAmber, "NOT FROM CRATES", 1.5f);
        PaintText(dl, g_fontBold, 17.0f, ImVec2(left + D(14.0f), y + D(32.0f)), kText, "Mission reward or DLC");
        PaintText(dl, nullptr, 13.0f, ImVec2(left + D(14.0f), y + D(58.0f)), kMuted, "No crate in any mission drops it.");
        return;
    }
    const Drop *best = drops[0];
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(left + D(14.0f), y + D(14.0f)), kGreen, "BEST FARM", 1.5f);
    char bestLine[160];
    snprintf(bestLine, sizeof(bestLine), "%s \xC2\xB7 %s %s", best->difficulty.c_str(), best->mission.c_str(),
             best->missionName.c_str());
    FittedText(dl, g_fontBold, 17.0f, ImVec2(left + D(14.0f), y + D(32.0f)), kText, bestLine, right - left - D(28.0f));
    char bestSub[96];
    snprintf(bestSub, sizeof(bestSub), "%.2f%% per crate \xC2\xB7 drops in %d mission%s", best->chance * 100.0f,
             (int)drops.size(), drops.size() == 1 ? "" : "s");
    PaintText(dl, nullptr, 13.0f, ImVec2(left + D(14.0f), y + D(58.0f)), kMuted, bestSub);
    y += D(100.0f);

    float cx = left;
    const float chipH = D(46.0f);
    for (int d = -1; d < kDifficultyCount; d++) {
        int count = 0;
        float bestChance = 0.0f;
        for (const Drop *drop : drops) {
            if (d == -1 || drop->difficulty == kDifficulties[d]) {
                count++;
                bestChance = drop->chance > bestChance ? drop->chance : bestChance;
            }
        }
        if (count == 0) {
            continue;
        }
        const char *title = d == -1 ? "All" : kDifficulties[d];
        char sub[48];
        if (d == -1) {
            snprintf(sub, sizeof(sub), "%d", count);
        } else {
            snprintf(sub, sizeof(sub), "%d \xC2\xB7 max %.2f%%", count, bestChance * 100.0f);
        }
        const float chipW = std::max(Measure(g_fontSemi, 14.0f, title).x, Measure(g_fontMono, 11.0f, sub).x) + D(20.0f);
        if (cx + chipW > right) {
            break;
        }
        ImGui::SetCursorScreenPos(ImVec2(cx, y));
        ImGui::PushID(d + 10);
        if (ImGui::InvisibleButton("chip", ImVec2(chipW, chipH))) {
            g.difficulty = d;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool selected = g.difficulty == d;
        if (selected || hovered) {
            dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + chipW, y + chipH), Rgb(kSelected));
        }
        dl->AddRect(ImVec2(cx, y), ImVec2(cx + chipW, y + chipH), Rgb(selected ? kSoft : kKeyLine), 0.0f, D(1.0f));
        PaintText(dl, g_fontSemi, 14.0f, ImVec2(cx + D(10.0f), y + D(6.0f)), d == -1 ? kText : kDifficultyColors[d], title);
        PaintText(dl, g_fontMono, 11.0f, ImVec2(cx + D(10.0f), y + D(26.0f)), kFaint, sub);
        cx += chipW + D(8.0f);
    }
    y += chipH + D(16.0f);

    const float colNum = left + D(88.0f);
    const float colName = left + D(130.0f);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(left, y), kFaint, "DIFF", 1.5f);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(colNum, y), kFaint, "#", 1.5f);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(colName, y), kFaint, "MISSION", 1.5f);
    const float chanceHeaderW = Measure(g_fontLabel, 11.0f, "CHANCE").x + D(1.5f) * 6;
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(right - chanceHeaderW, y), kFaint, "CHANCE", 1.5f);
    y += D(22.0f);

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("drops", ImVec2(w, bodyBottom - y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList *tdl = ImGui::GetWindowDrawList();
    const float offset = ImGui::GetCursorScreenPos().x - x;
    for (const Drop *drop : drops) {
        if (g.difficulty != -1 && drop->difficulty != kDifficulties[g.difficulty]) {
            continue;
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, D(kDropRowH)));
        const float rowH = D(kDropRowH);
        tdl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), Rgb(kRowLine), D(1.0f));
        const float ty = p.y + (rowH - Measure(nullptr, 15.0f, "Ag").y) * 0.5f;
        PaintText(tdl, nullptr, 15.0f, ImVec2(left + offset, ty), DifficultyColor(drop->difficulty),
                  drop->difficulty.c_str());
        PaintText(tdl, g_fontMono, 14.0f, ImVec2(colNum + offset, ty + D(1.0f)), kMuted, drop->mission.c_str());
        char chance[16];
        snprintf(chance, sizeof(chance), "%.2f%%", drop->chance * 100.0f);
        const ImVec2 cs = Measure(g_fontMono, 14.0f, chance);
        FittedText(tdl, nullptr, 15.0f, ImVec2(colName + offset, ty), kText, drop->missionName,
                   right - colName - cs.x - D(16.0f));
        const bool top = drop->chance >= best->chance * 0.98f;
        PaintText(tdl, g_fontMono, 14.0f, ImVec2(right + offset - cs.x, ty + D(1.0f)), top ? kGreen : kSoft, chance);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

float SpacedWidth(ImFont *f, float px, const char *t, float spacing) {
    int glyphs = 0;
    for (const char *c = t; *c; c++) {
        glyphs += ((unsigned char)*c & 0xC0) != 0x80 ? 1 : 0;
    }
    return Measure(f, px, t).x + D(spacing) * glyphs;
}

void HeaderLabel(ImDrawList *dl, float x, float y, const char *label) {
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(x, y), kFaint, label, 1.5f);
}

void HeaderLabelRight(ImDrawList *dl, float right, float y, const char *label) {
    HeaderLabel(dl, right - SpacedWidth(g_fontLabel, 11.0f, label, 1.5f), y, label);
}

void MonoRight(ImDrawList *dl, float right, float y, float px, uint32_t col, const char *t) {
    const ImVec2 s = Measure(g_fontMono, px, t);
    PaintText(dl, g_fontMono, px, ImVec2(right - s.x, y), col, t);
}

void CenteredMessage(ImDrawList *dl, float x, float w, float top, float bottom, const char *title, const char *hint) {
    const ImVec2 ts = Measure(g_fontBold, 18.0f, title);
    const float cy = top + (bottom - top) * 0.5f;
    PaintText(dl, g_fontBold, 18.0f, ImVec2(x + (w - ts.x) * 0.5f, cy - ts.y), kText, title);
    if (hint) {
        const ImVec2 hs = Measure(nullptr, 14.0f, hint);
        PaintText(dl, nullptr, 14.0f, ImVec2(x + (w - hs.x) * 0.5f, cy + D(6.0f)), kMuted, hint);
    }
}

void ViewTitle(ImDrawList *dl, ImVec2 o, float w, const char *title, const char *subtitle) {
    const float top = o.y + D(kHeaderH);
    PaintText(dl, g_fontBold, 22.0f, ImVec2(o.x + D(20.0f), top + D(10.0f)), kText, title);
    PaintText(dl, nullptr, 14.0f, ImVec2(o.x + D(20.0f), top + D(44.0f)), kMuted, subtitle);
    dl->AddLine(ImVec2(o.x, top + D(kStatsH)), ImVec2(o.x + w, top + D(kStatsH)), Rgb(kLine), D(1.0f));
}

bool InPool(const Weapon &w, const Drop &d) {
    return w.tier <= d.tier && d.lo <= w.level && w.level <= d.hi;
}

float CratesForAll(int pool, int missing) {
    double total = 0.0;
    for (int j = 1; j <= missing; j++) {
        total += 1.0 / j;
    }
    return (float)(pool * total);
}

struct PlanRow {
    const Drop *drop;
    int missing;
    int pool;
    float perCrate;
    float cratesOne;
    float cratesAll;
};

struct PlanCache {
    bool valid = false;
    unsigned version = 0;
    unsigned long long read = 0;
    int cls = -1;
    bool wishOnly = false;
    std::vector<PlanRow> rows;
};

PlanCache g_plan;

const std::vector<PlanRow> &PlanRows(const Catalog &cat) {
    const unsigned long long read = MomentoLecturaSave();
    if (g_plan.valid && g_plan.version == g.version && g_plan.read == read && g_plan.cls == g.farmClass &&
        g_plan.wishOnly == g.farmWishOnly) {
        return g_plan.rows;
    }
    g_plan.rows.clear();
    for (const Drop &d : GetDrops()) {
        int pool = 0;
        int missing = 0;
        for (int c = 0; c < (int)cat.classes.size(); c++) {
            const bool counted = g.farmClass == -1 || g.farmClass == c;
            for (const Weapon &w : cat.classes[c].weapons) {
                if (!InPool(w, d)) {
                    continue;
                }
                pool++;
                if (!w.owned && counted && (!g.farmWishOnly || w.wish)) {
                    missing++;
                }
            }
        }
        if (!missing || !pool) {
            continue;
        }
        PlanRow r{&d, missing, pool, (float)missing / pool, 0.0f, 0.0f};
        r.cratesOne = 1.0f / r.perCrate;
        r.cratesAll = CratesForAll(pool, missing);
        g_plan.rows.push_back(r);
    }
    std::sort(g_plan.rows.begin(), g_plan.rows.end(),
              [](const PlanRow &a, const PlanRow &b) { return a.perCrate > b.perCrate; });
    g_plan.valid = true;
    g_plan.version = g.version;
    g_plan.read = read;
    g_plan.cls = g.farmClass;
    g_plan.wishOnly = g.farmWishOnly;
    return g_plan.rows;
}

struct RouteStep {
    const Drop *drop;
    std::vector<const Weapon *> covers;
};

struct RouteCache {
    bool valid = false;
    unsigned version = 0;
    unsigned long long read = 0;
    std::vector<RouteStep> steps;
};

RouteCache g_route;

const std::vector<RouteStep> &Route(const Catalog &cat) {
    const unsigned long long read = MomentoLecturaSave();
    if (g_route.valid && g_route.version == g.version && g_route.read == read) {
        return g_route.steps;
    }
    g_route.steps.clear();
    std::vector<const Weapon *> pending;
    for (const ClassData &c : cat.classes) {
        for (const Weapon &w : c.weapons) {
            if (w.wish && !w.owned && w.farmable) {
                pending.push_back(&w);
            }
        }
    }
    while (!pending.empty() && g_route.steps.size() < 10) {
        const Drop *best = nullptr;
        std::vector<const Weapon *> bestCovers;
        for (const Drop &d : GetDrops()) {
            std::vector<const Weapon *> covers;
            for (const Weapon *w : pending) {
                if (InPool(*w, d)) {
                    covers.push_back(w);
                }
            }
            if (covers.empty()) {
                continue;
            }
            const bool wins = covers.size() > bestCovers.size() ||
                              (best && covers.size() == bestCovers.size() && d.chance > best->chance);
            if (wins) {
                best = &d;
                bestCovers = covers;
            }
        }
        if (!best) {
            break;
        }
        for (const Weapon *w : bestCovers) {
            pending.erase(std::remove(pending.begin(), pending.end(), w), pending.end());
        }
        g_route.steps.push_back(RouteStep{best, bestCovers});
    }
    g_route.valid = true;
    g_route.version = g.version;
    g_route.read = read;
    return g_route.steps;
}

void DrawFarmingBar(ImDrawList *dl, ImVec2 o, float w, const Catalog &cat) {
    ViewTitle(dl, o, w, "Where to farm next", "Chance that a crate gives you a weapon you don't have yet.");
    const float y = o.y + D(kHeaderH) + D(17.0f);
    const SegmentItem modes[] = {{"Everything missing", -1}, {"Wishlist only", -1}};
    float left = 0.0f;
    const int mode = SegmentGroup(dl, o.x + w - D(20.0f), y, modes, 2, g.farmWishOnly ? 1 : 0, 20, left);
    if (mode >= 0) {
        g.farmWishOnly = mode == 1;
    }
    std::vector<SegmentItem> classes;
    classes.push_back(SegmentItem{"All classes", -1});
    for (const ClassData &c : cat.classes) {
        classes.push_back(SegmentItem{c.name.c_str(), -1});
    }
    float classesLeft = 0.0f;
    const int cls = SegmentGroup(dl, left - D(16.0f), y, classes.data(), (int)classes.size(), g.farmClass + 1, 30,
                                 classesLeft);
    if (cls >= 0) {
        g.farmClass = cls - 1;
    }
}

void DrawRoute(ImDrawList *dl, float x, float w, float bodyTop, float bodyBottom, const Catalog &cat) {
    SpacedText(dl, g_fontLabel, 12.0f, ImVec2(x + D(24.0f), bodyTop + D(20.0f)), kFaint, "ROUTE", 1.5f);
    PaintText(dl, nullptr, 13.0f, ImVec2(x + D(24.0f), bodyTop + D(40.0f)), kMuted,
              "Fewest missions that cover your wishlist");
    const std::vector<RouteStep> &steps = Route(cat);
    const float listTop = bodyTop + D(70.0f);
    if (steps.empty()) {
        CenteredMessage(dl, x, w, listTop, bodyBottom, "No route",
                        EnWishlist() == 0 ? "Mark weapons with W or the heart." : "Nothing on your wishlist drops from a crate.");
        return;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, listTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("route", ImVec2(w, bodyBottom - listTop), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImDrawList *rdl = ImGui::GetWindowDrawList();
    const float innerW = ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i < steps.size(); i++) {
        const RouteStep &s = steps[i];
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float left = p.x + D(24.0f);
        const ImVec2 box(left, p.y + D(2.0f));
        rdl->AddRect(box, ImVec2(box.x + D(24.0f), box.y + D(24.0f)), Rgb(kKeyLine), 0.0f, D(1.0f));
        char n[8];
        snprintf(n, sizeof(n), "%d", (int)i + 1);
        const ImVec2 ns = Measure(g_fontMono, 13.0f, n);
        PaintText(rdl, g_fontMono, 13.0f, ImVec2(box.x + (D(24.0f) - ns.x) * 0.5f, box.y + (D(24.0f) - ns.y) * 0.5f),
                  kSoft, n);
        char title[160];
        snprintf(title, sizeof(title), "%s \xC2\xB7 %s %s", s.drop->difficulty.c_str(), s.drop->mission.c_str(),
                 s.drop->missionName.c_str());
        FittedText(rdl, g_fontSemi, 15.0f, ImVec2(left + D(36.0f), p.y + D(2.0f)), kText, title,
                   innerW - D(36.0f) - D(48.0f));
        char sub[64];
        snprintf(sub, sizeof(sub), "%.2f%% per crate \xC2\xB7 covers %d", s.drop->chance * 100.0f, (int)s.covers.size());
        PaintText(rdl, nullptr, 13.0f, ImVec2(left + D(36.0f), p.y + D(24.0f)), kMuted, sub);
        float y = p.y + D(50.0f);
        for (const Weapon *weapon : s.covers) {
            Heart(rdl, ImVec2(left + D(44.0f), y + D(10.0f)), D(12.0f), kPink);
            FittedText(rdl, nullptr, 14.0f, ImVec2(left + D(58.0f), y), kSoft, weapon->name, innerW - D(58.0f) - D(48.0f));
            y += D(24.0f);
        }
        ImGui::Dummy(ImVec2(innerW, y - p.y + D(14.0f)));
    }
    ImGui::EndChild();
}

void DrawFarming(ImDrawList *dl, ImVec2 o, float w, float bodyTop, float bodyBottom, const Catalog &cat) {
    const float routeW = g.farmWishOnly ? D(460.0f) : 0.0f;
    const float x = o.x;
    const float tableW = w - routeW;
    const float right = x + tableW - ImGui::GetStyle().ScrollbarSize - D(20.0f);
    const float colNum = x + D(120.0f);
    const float colName = x + D(170.0f);
    const float colMissing = right - D(470.0f);
    const float colPool = right - D(390.0f);
    const float colPer = right - D(260.0f);
    const float colOne = right - D(130.0f);
    const float headerY = bodyTop + D(11.0f);
    HeaderLabel(dl, x + D(20.0f), headerY, "DIFF");
    HeaderLabel(dl, colNum, headerY, "#");
    HeaderLabel(dl, colName, headerY, "MISSION");
    HeaderLabelRight(dl, colMissing, headerY, g.farmWishOnly ? "WANTED" : "MISSING");
    HeaderLabelRight(dl, colPool, headerY, "POOL");
    HeaderLabelRight(dl, colPer, headerY, "NEW PER CRATE");
    HeaderLabelRight(dl, colOne, headerY, "CRATES FOR 1");
    HeaderLabelRight(dl, right, headerY, "FOR ALL");
    dl->AddLine(ImVec2(x, bodyTop + D(kListHeaderH)), ImVec2(x + tableW, bodyTop + D(kListHeaderH)), Rgb(kLine),
                D(1.0f));
    if (routeW > 0.0f) {
        dl->AddLine(ImVec2(x + tableW, bodyTop), ImVec2(x + tableW, bodyBottom), Rgb(kLine), D(1.0f));
        DrawRoute(dl, x + tableW, routeW, bodyTop, bodyBottom, cat);
    }

    const std::vector<PlanRow> &rows = PlanRows(cat);
    const float listTop = bodyTop + D(kListHeaderH) + D(1.0f);
    if (rows.empty()) {
        const char *why = "Nothing left to find in any crate.";
        if (g.farmWishOnly) {
            why = EnWishlist() == 0 ? "Your wishlist is empty: mark weapons with W or the heart."
                                    : "Nothing on your wishlist drops from a crate.";
        }
        CenteredMessage(dl, x, tableW, listTop, bodyBottom, "No missions to suggest", why);
        return;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, listTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("plan", ImVec2(tableW, bodyBottom - listTop), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList *pdl = ImGui::GetWindowDrawList();
    const float rowH = D(kRowH);
    const float rowW = ImGui::GetContentRegionAvail().x;
    ImGuiListClipper clipper;
    clipper.Begin((int)rows.size(), rowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const PlanRow &r = rows[i];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(rowW, rowH));
            pdl->AddLine(ImVec2(p.x, p.y + rowH), ImVec2(p.x + rowW, p.y + rowH), Rgb(kRowLine), D(1.0f));
            const float ty = p.y + (rowH - Measure(nullptr, 15.0f, "Ag").y) * 0.5f;
            const float my = p.y + (rowH - Measure(g_fontMono, 14.0f, "0").y) * 0.5f;
            PaintText(pdl, nullptr, 15.0f, ImVec2(x + D(20.0f), ty), DifficultyColor(r.drop->difficulty),
                      r.drop->difficulty.c_str());
            PaintText(pdl, g_fontMono, 14.0f, ImVec2(colNum, my), kMuted, r.drop->mission.c_str());
            FittedText(pdl, g_fontSemi, 15.0f, ImVec2(colName, ty), kText, r.drop->missionName,
                       colMissing - colName - D(90.0f));
            char v[32];
            snprintf(v, sizeof(v), "%d", r.missing);
            MonoRight(pdl, colMissing, my, 14.0f, g.farmWishOnly ? kPink : kText, v);
            snprintf(v, sizeof(v), "%d", r.pool);
            MonoRight(pdl, colPool, my, 14.0f, kFaint, v);
            snprintf(v, sizeof(v), "%.1f%%", r.perCrate * 100.0f);
            MonoRight(pdl, colPer, my, 14.0f, i < 3 ? kGreen : kSoft, v);
            snprintf(v, sizeof(v), "%.1f", r.cratesOne);
            MonoRight(pdl, colOne, my, 14.0f, kSoft, v);
            snprintf(v, sizeof(v), "%.0f", r.cratesAll);
            MonoRight(pdl, right, my, 14.0f, kFaint, v);
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

struct MissionRow {
    const Drop *drop;
    int missing;
    int pool;
};

struct MissionCache {
    bool valid = false;
    unsigned long long read = 0;
    int difficulty = -1;
    std::vector<MissionRow> rows;
};

MissionCache g_missions;

const std::vector<MissionRow> &MissionRows(const Catalog &cat) {
    const unsigned long long read = MomentoLecturaSave();
    if (g_missions.valid && g_missions.read == read && g_missions.difficulty == g.missionDifficulty) {
        return g_missions.rows;
    }
    g_missions.rows.clear();
    for (const Drop &d : GetDrops()) {
        if (d.difficulty != kDifficulties[g.missionDifficulty]) {
            continue;
        }
        MissionRow r{&d, 0, 0};
        for (const ClassData &c : cat.classes) {
            for (const Weapon &w : c.weapons) {
                if (InPool(w, d)) {
                    r.pool++;
                    r.missing += w.owned ? 0 : 1;
                }
            }
        }
        g_missions.rows.push_back(r);
    }
    g_missions.valid = true;
    g_missions.read = read;
    g_missions.difficulty = g.missionDifficulty;
    return g_missions.rows;
}

void DrawMissionsBar(ImDrawList *dl, ImVec2 o, float w) {
    ViewTitle(dl, o, w, "What each mission still has for you",
              "Missing weapons in each mission's crate pool, across all classes.");
    const SegmentItem items[] = {{"Normal", -1}, {"Hard", -1}, {"Hardest", -1}, {"Inferno", -1}};
    float left = 0.0f;
    const int clicked = SegmentGroup(dl, o.x + w - D(20.0f), o.y + D(kHeaderH) + D(17.0f), items, kDifficultyCount,
                                     g.missionDifficulty, 40, left);
    if (clicked >= 0) {
        g.missionDifficulty = clicked;
    }
}

void DrawGaps(ImDrawList *dl, ImVec2 p, float width, float height, float lo, float hi, const Catalog &cat) {
    constexpr int kBands = 12;
    int missing[kBands] = {0};
    int maxv = 1;
    for (const ClassData &c : cat.classes) {
        for (const Weapon &w : c.weapons) {
            if (w.owned || !w.farmable) {
                continue;
            }
            const int band = std::min(w.level / 10, kBands - 1);
            missing[band]++;
        }
    }
    for (int v : missing) {
        maxv = std::max(maxv, v);
    }
    const float bandW = width / kBands;
    for (int i = 0; i < kBands; i++) {
        const bool inside = (float)(i * 10 + 9) >= lo && (float)(i * 10) <= hi;
        const float h = height * missing[i] / maxv;
        const ImVec2 a(p.x + i * bandW + D(3.0f), p.y + height - h);
        const ImVec2 b(p.x + (i + 1) * bandW - D(3.0f), p.y + height);
        dl->AddRectFilled(a, b, Rgb(inside ? kGreen : kKeyLine, inside ? 0.85f : 1.0f));
        char n[8];
        if (missing[i] > 0) {
            snprintf(n, sizeof(n), "%d", missing[i]);
            const ImVec2 ns = Measure(g_fontMono, 12.0f, n);
            PaintText(dl, g_fontMono, 12.0f, ImVec2(a.x + (b.x - a.x - ns.x) * 0.5f, a.y - ns.y - D(2.0f)),
                      inside ? kText : kMuted, n);
        }
        snprintf(n, sizeof(n), "%d", i * 10);
        const ImVec2 ls = Measure(g_fontMono, 11.0f, n);
        PaintText(dl, g_fontMono, 11.0f, ImVec2(a.x + (b.x - a.x - ls.x) * 0.5f, p.y + height + D(4.0f)), kFaint, n);
    }
}

void MoveInList(const std::vector<std::string> &keys, std::string &current) {
    if (keys.empty() || ImGui::GetIO().WantTextInput) {
        return;
    }
    const int step = ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0);
    if (!step) {
        return;
    }
    int pos = 0;
    while (pos < (int)keys.size() && keys[pos] != current) {
        pos++;
    }
    pos = pos >= (int)keys.size() ? 0 : std::min(std::max(pos + step, 0), (int)keys.size() - 1);
    current = keys[pos];
}

void DrawMissions(ImDrawList *dl, ImVec2 o, float w, float bodyTop, float bodyBottom, Catalog &cat) {
    const std::vector<MissionRow> &rows = MissionRows(cat);
    if (rows.empty()) {
        CenteredMessage(dl, o.x, w, bodyTop, bodyBottom, "missions.tsv not loaded",
                        "Expected at Mods\\Compendium\\missions.tsv");
        return;
    }
    std::vector<std::string> keys;
    const MissionRow *selected = nullptr;
    for (const MissionRow &r : rows) {
        keys.push_back(r.drop->mission);
    }
    MoveInList(keys, g.mission);
    for (const MissionRow &r : rows) {
        if (r.drop->mission == g.mission) {
            selected = &r;
        }
    }
    if (!selected) {
        selected = &rows[0];
        g.mission = selected->drop->mission;
    }

    const float listW = D(460.0f);
    dl->AddLine(ImVec2(o.x + listW, bodyTop), ImVec2(o.x + listW, bodyBottom), Rgb(kLine), D(1.0f));
    ImGui::SetCursorScreenPos(ImVec2(o.x, bodyTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("missions", ImVec2(listW - D(1.0f), bodyBottom - bodyTop), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList *ldl = ImGui::GetWindowDrawList();
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float rowH = D(44.0f);
    for (const MissionRow &r : rows) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(r.drop);
        if (ImGui::InvisibleButton("mission", ImVec2(rowW, rowH))) {
            g.mission = r.drop->mission;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool isSelected = &r == selected;
        if (isSelected || hovered) {
            ldl->AddRectFilled(p, ImVec2(p.x + rowW, p.y + rowH), Rgb(kSelected, isSelected ? 1.0f : 0.5f));
        }
        const float ty = p.y + (rowH - Measure(g_fontSemi, 15.0f, "Ag").y) * 0.5f;
        const float my = p.y + (rowH - Measure(g_fontMono, 13.0f, "0").y) * 0.5f;
        PaintText(ldl, g_fontMono, 13.0f, ImVec2(p.x + D(20.0f), my), kMuted, r.drop->mission.c_str());
        char count[24];
        snprintf(count, sizeof(count), "%d/%d", r.missing, r.pool);
        const ImVec2 cs = Measure(g_fontMono, 13.0f, count);
        FittedText(ldl, g_fontSemi, 15.0f, ImVec2(p.x + D(64.0f), ty), kText, r.drop->missionName,
                   rowW - D(64.0f) - cs.x - D(40.0f));
        PaintText(ldl, g_fontMono, 13.0f, ImVec2(p.x + rowW - D(20.0f) - cs.x, my), r.missing ? kGreen : kFaint, count);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    const Drop &d = *selected->drop;
    const float x = o.x + listW + D(24.0f);
    const float right = o.x + w - D(24.0f);
    char title[160];
    snprintf(title, sizeof(title), "%s \xC2\xB7 %s", d.mission.c_str(), d.missionName.c_str());
    FittedText(dl, g_fontBold, 22.0f, ImVec2(x, bodyTop + D(18.0f)), kText, title, right - x);
    char sub[160];
    snprintf(sub, sizeof(sub), "%s \xC2\xB7 levels %.0f-%.0f \xC2\xB7 %d of %d missing \xC2\xB7 %.2f%% per crate",
             d.difficulty.c_str(), d.lo, d.hi, selected->missing, selected->pool, d.chance * 100.0f);
    PaintText(dl, nullptr, 14.0f, ImVec2(x, bodyTop + D(50.0f)), kMuted, sub);

    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(x, bodyTop + D(86.0f)), kFaint, "YOUR GAPS BY LEVEL", 1.5f);
    PaintText(dl, nullptr, 12.0f, ImVec2(x + D(170.0f), bodyTop + D(84.0f)), kFaint, "(green: this mission's range)");
    DrawGaps(dl, ImVec2(x, bodyTop + D(122.0f)), right - x, D(80.0f), d.lo, d.hi, cat);

    const float headerY = bodyTop + D(236.0f);
    HeaderLabel(dl, x + D(28.0f), headerY, "MISSING HERE");
    HeaderLabelRight(dl, right - D(150.0f), headerY, "LV");
    HeaderLabel(dl, right - D(130.0f), headerY, "CLASS");
    const float listTop = headerY + D(24.0f);
    dl->AddLine(ImVec2(o.x + listW, listTop), ImVec2(o.x + w, listTop), Rgb(kLine), D(1.0f));

    struct Missing {
        Weapon *weapon;
        const char *cls;
    };
    std::vector<Missing> missing;
    for (ClassData &c : cat.classes) {
        for (Weapon &weapon : c.weapons) {
            if (!weapon.owned && InPool(weapon, d)) {
                missing.push_back(Missing{&weapon, c.name.c_str()});
            }
        }
    }
    if (missing.empty()) {
        CenteredMessage(dl, o.x + listW, w - listW, listTop, bodyBottom, "Nothing missing here",
                        "Every weapon in this mission's pool is already yours.");
        return;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x + listW, listTop + D(1.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("missingHere", ImVec2(w - listW, bodyBottom - listTop - D(1.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList *mdl = ImGui::GetWindowDrawList();
    const float mRowW = ImGui::GetContentRegionAvail().x;
    const float mRowH = D(36.0f);
    ImGuiListClipper clipper;
    clipper.Begin((int)missing.size(), mRowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            Weapon &weapon = *missing[i].weapon;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("wish", ImVec2(mRowW, mRowH))) {
                ToggleWish(weapon);
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hovered) {
                mdl->AddRectFilled(p, ImVec2(p.x + mRowW, p.y + mRowH), Rgb(kSelected, 0.5f));
            }
            Heart(mdl, ImVec2(x + D(8.0f), p.y + mRowH * 0.5f), D(14.0f), weapon.wish ? kPink : (hovered ? kMuted : kKeyLine));
            const float ty = p.y + (mRowH - Measure(g_fontSemi, 15.0f, "Ag").y) * 0.5f;
            FittedText(mdl, g_fontSemi, 15.0f, ImVec2(x + D(28.0f), ty), kText, weapon.name,
                       right - D(150.0f) - x - D(28.0f) - D(50.0f));
            char lv[8];
            snprintf(lv, sizeof(lv), "%d", weapon.level);
            MonoRight(mdl, right - D(150.0f), p.y + (mRowH - Measure(g_fontMono, 14.0f, "0").y) * 0.5f, 14.0f, kSoft, lv);
            PaintText(mdl, nullptr, 14.0f, ImVec2(right - D(130.0f), ty), kMuted, missing[i].cls);
            if (hovered && !weapon.stats.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ShowStatsTooltip(weapon, weapon.wish ? "Click to remove it from the wishlist."
                                                     : "Click to add it to the wishlist.");
            }
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void DrawStratsBar(ImDrawList *dl, ImVec2 o, float w) {
    ViewTitle(dl, o, w, "Community strategies",
              "Mission numbers are checked against the game data; the yields each guide claims are not.");
}

void DrawStrats(ImDrawList *dl, ImVec2 o, float w, float bodyTop, float bodyBottom, const Catalog &cat) {
    const std::vector<Strat> &strats = GetStrats();
    if (strats.empty()) {
        CenteredMessage(dl, o.x, w, bodyTop, bodyBottom, "strats.tsv not loaded",
                        "Expected at Mods\\Compendium\\strats.tsv");
        return;
    }
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            g.strat = std::max(0, g.strat - 1);
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            g.strat = std::min((int)strats.size() - 1, g.strat + 1);
        }
    }
    if (g.strat >= (int)strats.size()) {
        g.strat = 0;
    }

    const float listW = D(460.0f);
    dl->AddLine(ImVec2(o.x + listW, bodyTop), ImVec2(o.x + listW, bodyBottom), Rgb(kLine), D(1.0f));
    ImGui::SetCursorScreenPos(ImVec2(o.x, bodyTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("strats", ImVec2(listW - D(1.0f), bodyBottom - bodyTop), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList *ldl = ImGui::GetWindowDrawList();
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float rowH = D(60.0f);
    for (int i = 0; i < (int)strats.size(); i++) {
        const Strat &s = strats[i];
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("strat", ImVec2(rowW, rowH))) {
            g.strat = i;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (i == g.strat || hovered) {
            ldl->AddRectFilled(p, ImVec2(p.x + rowW, p.y + rowH), Rgb(kSelected, i == g.strat ? 1.0f : 0.5f));
        }
        FittedText(ldl, g_fontSemi, 15.0f, ImVec2(p.x + D(20.0f), p.y + D(10.0f)), kText, s.title, rowW - D(40.0f));
        char meta[128];
        if (s.mission == "any") {
            snprintf(meta, sizeof(meta), "General");
        } else {
            snprintf(meta, sizeof(meta), "Mission %s \xC2\xB7 %s \xC2\xB7 %s", s.mission.c_str(), s.difficulty.c_str(),
                     s.className.c_str());
        }
        PaintText(ldl, nullptr, 12.0f, ImVec2(p.x + D(20.0f), p.y + D(34.0f)), kMuted, meta);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    const Strat &s = strats[g.strat];
    ImGui::SetCursorScreenPos(ImVec2(o.x + listW, bodyTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("strat", ImVec2(w - listW, bodyBottom - bodyTop), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImDrawList *sdl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float innerW = ImGui::GetContentRegionAvail().x;
    const float x = start.x + D(28.0f);
    const float wrap = innerW - D(56.0f);
    float y = start.y + D(22.0f);
    y += WrappedText(sdl, g_fontBold, 22.0f, ImVec2(x, y), kText, s.title.c_str(), wrap) + D(6.0f);
    if (s.mission != "any") {
        char meta[128];
        snprintf(meta, sizeof(meta), "Mission %s \xC2\xB7 %s \xC2\xB7 %s", s.mission.c_str(), s.difficulty.c_str(),
                 s.className.c_str());
        y += WrappedText(sdl, nullptr, 14.0f, ImVec2(x, y), kMuted, meta, wrap) + D(12.0f);
    }
    sdl->AddLine(ImVec2(start.x, y), ImVec2(start.x + innerW, y), Rgb(kLine), D(1.0f));
    y += D(16.0f);
    y += WrappedText(sdl, nullptr, 15.0f, ImVec2(x, y), kSoft, s.body.c_str(), wrap) + D(20.0f);

    if (s.mission != "any" && !s.mission.empty()) {
        bool header = false;
        for (const Drop &d : GetDrops()) {
            if (d.mission != s.mission) {
                continue;
            }
            int pool = 0;
            int missing = 0;
            for (const ClassData &c : cat.classes) {
                for (const Weapon &weapon : c.weapons) {
                    if (InPool(weapon, d)) {
                        pool++;
                        missing += weapon.owned ? 0 : 1;
                    }
                }
            }
            if (!pool) {
                continue;
            }
            if (!header) {
                char label[64];
                snprintf(label, sizeof(label), "YOUR PROGRESS ON MISSION %s", s.mission.c_str());
                SpacedText(sdl, g_fontLabel, 11.0f, ImVec2(x, y), kFaint, label, 1.5f);
                y += D(24.0f);
                header = true;
            }
            PaintText(sdl, nullptr, 14.0f, ImVec2(x, y), DifficultyColor(d.difficulty), d.difficulty.c_str());
            char line[96];
            snprintf(line, sizeof(line), "%d of %d missing \xC2\xB7 %.0f%% new per crate", missing, pool,
                     100.0f * missing / pool);
            PaintText(sdl, nullptr, 14.0f, ImVec2(x + D(90.0f), y), missing ? kSoft : kFaint, line);
            y += D(24.0f);
        }
        if (header) {
            y += D(14.0f);
        }
    }
    if (!s.source.empty()) {
        const std::string source = "Source: " + s.source;
        y += WrappedText(sdl, nullptr, 13.0f, ImVec2(x, y), kFaint, source.c_str(), wrap);
    }
    ImGui::Dummy(ImVec2(innerW, y - start.y + D(20.0f)));
    ImGui::EndChild();
}

void MoveSelection(const std::vector<Row> &rows, int step) {
    std::vector<int> items;
    for (const Row &r : rows) {
        if (!r.header) {
            items.push_back(r.weapon->index);
        }
    }
    if (items.empty()) {
        return;
    }
    int pos = -1;
    for (int i = 0; i < (int)items.size(); i++) {
        if (items[i] == g.selected) {
            pos = i;
        }
    }
    pos = pos < 0 ? 0 : std::min(std::max(pos + step, 0), (int)items.size() - 1);
    SelectWeapon(items[pos]);
    g.scrollToSelected = true;
}

void HandleGlobalKeys(Catalog &cat) {
    if (ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g.search[0] = '\0';
            ImGui::SetWindowFocus(nullptr);
        }
        return;
    }
    const int classCount = (int)cat.classes.size();
    if (classCount == 0) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        SwitchTab(CurrentTab(classCount) - 1, classCount);
    } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        SwitchTab(CurrentTab(classCount) + 1, classCount);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Slash, false) && g.view == View::Class) {
        g.focusSearch = true;
    }
}

void HandleListKeys(const std::vector<Row> &rows) {
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        MoveSelection(rows, -1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        MoveSelection(rows, 1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_W, false) && g.selected >= 0) {
        if (Weapon *w = ArmaPorIndice(g.selected)) {
            ToggleWish(*w);
        }
    }
}

bool SelectedIsVisible(const std::vector<Row> &rows) {
    for (const Row &r : rows) {
        if (!r.header && r.weapon->index == g.selected) {
            return true;
        }
    }
    return false;
}

}

void DrawCompendiumPanel(bool &open, float scale) {
    Catalog &cat = MutableCatalog();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    float k = scale * kDesignScale;
    if (kWindowW * k > screen.x * 0.97f) {
        k = screen.x * 0.97f / kWindowW;
    }
    if (kWindowH * k > screen.y * 0.94f) {
        k = screen.y * 0.94f / kWindowH;
    }
    SetScale(k);
    const ImVec2 size(D(kWindowW), D(kWindowH));

    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, D(1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Rgb(kBg, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, Rgb(kLine));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::Begin("##compendium", &open, flags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (!visible) {
        ImGui::End();
        return;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    if (cat.classes.empty()) {
        const char *msg = "weapons.tsv not loaded (expected at Mods\\Compendium\\weapons.tsv)";
        const ImVec2 ms = Measure(nullptr, 15.0f, msg);
        PaintText(dl, nullptr, 15.0f, ImVec2(o.x + (size.x - ms.x) * 0.5f, o.y + size.y * 0.5f), kDanger, msg);
        ImGui::End();
        return;
    }
    HandleGlobalKeys(cat);
    const float bodyTop = o.y + D(kHeaderH) + D(kStatsH) + D(1.0f);
    const float bodyBottom = o.y + size.y;
    if (g.view != View::Class) {
        DrawHeader(dl, o, size.x, cat, open);
        switch (g.view) {
        case View::Farming:
            DrawFarmingBar(dl, o, size.x, cat);
            DrawFarming(dl, o, size.x, bodyTop, bodyBottom, cat);
            break;
        case View::Missions:
            DrawMissionsBar(dl, o, size.x);
            DrawMissions(dl, o, size.x, bodyTop, bodyBottom, cat);
            break;
        case View::Strats:
            DrawStratsBar(dl, o, size.x);
            DrawStrats(dl, o, size.x, bodyTop, bodyBottom, cat);
            break;
        case View::Class:
            break;
        }
        ImGui::End();
        return;
    }
    if (g.viewedClass >= (int)cat.classes.size()) {
        g.viewedClass = 0;
    }
    ClassData &cls = cat.classes[g.viewedClass];
    const std::vector<Category> categories = CategoriesOf(cls);
    bool categoryExists = g.category == -1;
    for (const Category &c : categories) {
        categoryExists = categoryExists || c.id == g.category;
    }
    if (!categoryExists) {
        g.category = -1;
    }
    std::vector<Row> rows = BuildRows(cls, categories);
    if (!SelectedIsVisible(rows)) {
        for (const Row &r : rows) {
            if (!r.header) {
                SelectWeapon(r.weapon->index);
                break;
            }
        }
    }
    HandleListKeys(rows);

    const Counts classCounts = CountOf(cls.weapons, -1);
    DrawHeader(dl, o, size.x, cat, open);
    DrawStats(dl, o, size.x, classCounts);
    const float listX = o.x + D(kSidebarW);
    const float detailX = o.x + size.x - D(kDetailW);
    dl->AddLine(ImVec2(listX, bodyTop), ImVec2(listX, bodyBottom), Rgb(kLine), D(1.0f));
    dl->AddLine(ImVec2(detailX, bodyTop), ImVec2(detailX, bodyBottom), Rgb(kLine), D(1.0f));
    DrawSidebar(o, bodyTop, bodyBottom, categories, classCounts);
    DrawList(dl, listX, detailX - listX, bodyTop, bodyBottom, rows);
    DrawDetail(dl, detailX, o.x + size.x - detailX, bodyTop, bodyBottom, cls);
    ImGui::End();
}
