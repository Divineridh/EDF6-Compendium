#include <windows.h>

#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

#include "compendium.h"
#include "loadouts.h"

namespace {

constexpr int kWeaponSlotsPerClass = 4;
constexpr int kHeaderDwords = 2;
constexpr int kTableDwords = kHeaderDwords + kClassCount * kSlotsPerClass;
constexpr uint32_t kTerminator = 0xFFFFFFFF;
constexpr int kMaxCandidates = 8;
constexpr DWORD kRescanMs = 5000;
constexpr DWORD kPollMs = 500;
constexpr double kStatusSeconds = 8.0;
constexpr const char *kLoadoutsFile = "Mods\\Compendium\\loadouts.tsv";

const char *const kClassNames[kClassCount] = {"Ranger", "Wing Diver", "Air Raider", "Fencer"};
const char *const kSlotLabels[kSlotsPerClass] = {"W1", "W2", "W3", "W4", "S1", "S2"};

struct SavedLoadout {
    int classId = 0;
    std::string title;
    int slots[kSlotsPerClass] = {};
    std::string names[kSlotsPerClass];
    bool outdated = false;
};

std::vector<int> g_classOfWeapon;
std::atomic<uint32_t *> g_table{nullptr};

std::mutex g_snapshotMutex;
Equipment g_snapshot;
bool g_snapshotValid = false;

std::vector<SavedLoadout> g_loadouts;
std::string g_status;
bool g_statusIsError = false;
double g_statusUntil = 0.0;

void PublishSnapshot(const Equipment *e) {
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    g_snapshotValid = e != nullptr;
    if (e) {
        g_snapshot = *e;
    }
}

bool LooksLikeTable(const uint32_t *p) {
    if (p[0] >= kClassCount || p[kTableDwords] != kTerminator) {
        return false;
    }
    const uint32_t weaponCount = (uint32_t)g_classOfWeapon.size();
    for (int c = 0; c < kClassCount; c++) {
        for (int s = 0; s < kSlotsPerClass; s++) {
            const uint32_t weapon = p[kHeaderDwords + c * kSlotsPerClass + s];
            if (weapon >= weaponCount) {
                return false;
            }
            if (s < kWeaponSlotsPerClass && g_classOfWeapon[weapon] != c) {
                return false;
            }
        }
    }
    return true;
}

bool SlotsFitClass(int classId, const int slots[kSlotsPerClass]) {
    for (int s = 0; s < kSlotsPerClass; s++) {
        if (slots[s] < 0 || slots[s] >= (int)g_classOfWeapon.size()) {
            return false;
        }
        if (s < kWeaponSlotsPerClass && g_classOfWeapon[slots[s]] != classId) {
            return false;
        }
    }
    return true;
}

bool ScanRegion(uint32_t *begin, size_t count, uint32_t **found, int &foundCount) {
    const uint32_t weaponCount = (uint32_t)g_classOfWeapon.size();
    __try {
        size_t run = 0;
        for (size_t i = 0; i < count; i++) {
            const uint32_t v = begin[i];
            if (v == kTerminator) {
                if (run >= (size_t)kTableDwords && LooksLikeTable(begin + i - kTableDwords) &&
                    foundCount < kMaxCandidates) {
                    found[foundCount++] = begin + i - kTableDwords;
                }
                run = 0;
            } else if (v < weaponCount) {
                run++;
            } else {
                run = 0;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t *FindTable() {
    const DWORD start = GetTickCount();
    uint32_t *found[kMaxCandidates] = {};
    int foundCount = 0;
    size_t scannedBytes = 0;
    int faultedRegions = 0;

    MEMORY_BASIC_INFORMATION mbi;
    char *addr = nullptr;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        char *base = (char *)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            if (!ScanRegion((uint32_t *)base, mbi.RegionSize / 4, found, foundCount)) {
                faultedRegions++;
            }
            scannedBytes += mbi.RegionSize;
        }
        addr = base + mbi.RegionSize;
    }

    if (foundCount > 0) {
        LogF("loadouts: tabla en %p (%d candidatos, %lu ms, %zu MB, %d regiones con fallo)", found[0],
             foundCount, GetTickCount() - start, scannedBytes >> 20, faultedRegions);
        return found[0];
    }
    return nullptr;
}

bool CopyIfValid(const uint32_t *table, Equipment &out) {
    __try {
        if (!LooksLikeTable(table)) {
            return false;
        }
        out.activeClass = (int)table[0];
        for (int c = 0; c < kClassCount; c++) {
            for (int s = 0; s < kSlotsPerClass; s++) {
                out.slots[c][s] = (int)table[kHeaderDwords + c * kSlotsPerClass + s];
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteIfValid(uint32_t *table, int classId, const int slots[kSlotsPerClass]) {
    __try {
        if (!LooksLikeTable(table)) {
            return false;
        }
        for (int s = 0; s < kSlotsPerClass; s++) {
            table[kHeaderDwords + classId * kSlotsPerClass + s] = (uint32_t)slots[s];
        }
        table[0] = (uint32_t)classId;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void LogEquipment(const Equipment &e) {
    LogF("loadouts: clase activa %s", kClassNames[e.activeClass]);
    for (int c = 0; c < kClassCount; c++) {
        std::string line = kClassNames[c];
        line += ":";
        for (int s = 0; s < kSlotsPerClass; s++) {
            const Weapon *w = ArmaPorIndice(e.slots[c][s]);
            line += s == kWeaponSlotsPerClass ? " || " : (s == 0 ? " " : " | ");
            line += w ? w->name : std::to_string(e.slots[c][s]);
        }
        LogF("loadouts:   %s", line.c_str());
    }
}

bool ReadEquipment(Equipment &out) {
    uint32_t *table = g_table.load();
    if (table && CopyIfValid(table, out)) {
        return true;
    }
    if (table) {
        Log("loadouts: la tabla dejo de ser valida en la direccion guardada; busco de nuevo");
        g_table = nullptr;
    }
    table = FindTable();
    if (!table) {
        return false;
    }
    g_table = table;
    return CopyIfValid(table, out);
}

DWORD WINAPI WatchThread(LPVOID) {
    Equipment last;
    bool haveLast = false;
    bool reportedMissing = false;
    for (;;) {
        Equipment now;
        if (!ReadEquipment(now)) {
            PublishSnapshot(nullptr);
            if (!reportedMissing) {
                Log("loadouts: tabla no encontrada; reintento cada 5 s (normal antes de cargar la partida)");
                reportedMissing = true;
                haveLast = false;
            }
            Sleep(kRescanMs);
            continue;
        }
        reportedMissing = false;
        PublishSnapshot(&now);
        if (!haveLast || memcmp(&now, &last, sizeof(now)) != 0) {
            LogEquipment(now);
            last = now;
            haveLast = true;
        }
        Sleep(kPollMs);
    }
}

std::string CleanTitle(const char *raw) {
    std::string title = raw;
    for (char &ch : title) {
        if (ch == '\t' || ch == '\r' || ch == '\n') {
            ch = ' ';
        }
    }
    const size_t first = title.find_first_not_of(' ');
    const size_t last = title.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : title.substr(first, last - first + 1);
}

int ClassIdByName(const std::string &name) {
    for (int c = 0; c < kClassCount; c++) {
        if (name == kClassNames[c]) {
            return c;
        }
    }
    return -1;
}

void MarkOutdated(SavedLoadout &l) {
    l.outdated = !SlotsFitClass(l.classId, l.slots);
    for (int s = 0; s < kSlotsPerClass && !l.outdated; s++) {
        const Weapon *w = ArmaPorIndice(l.slots[s]);
        l.outdated = !w || w->name != l.names[s];
    }
}

void LoadSavedLoadouts() {
    const std::string path = GamePath(kLoadoutsFile);
    std::ifstream in(path);
    if (!in) {
        LogF("loadouts: sin %s todavia", kLoadoutsFile);
        return;
    }
    std::string line;
    int outdated = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::istringstream ss(line);
        std::string field;
        while (std::getline(ss, field, '\t')) {
            fields.push_back(field);
        }
        if (fields.size() != 2 + 2 * kSlotsPerClass || ClassIdByName(fields[0]) < 0) {
            LogF("loadouts: linea ignorada en %s: %s", kLoadoutsFile, line.c_str());
            continue;
        }
        SavedLoadout l;
        l.classId = ClassIdByName(fields[0]);
        l.title = fields[1];
        for (int s = 0; s < kSlotsPerClass; s++) {
            l.slots[s] = atoi(fields[2 + 2 * s].c_str());
            l.names[s] = fields[3 + 2 * s];
        }
        MarkOutdated(l);
        outdated += l.outdated ? 1 : 0;
        g_loadouts.push_back(l);
    }
    LogF("loadouts: %d guardados leidos, %d desactualizados", (int)g_loadouts.size(), outdated);
}

bool WriteSavedLoadouts() {
    const std::string path = GamePath(kLoadoutsFile);
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) {
            return false;
        }
        out << "# class\ttitle\tthen index and weapon name for each of the 6 slots\n";
        for (const SavedLoadout &l : g_loadouts) {
            out << kClassNames[l.classId] << '\t' << l.title;
            for (int s = 0; s < kSlotsPerClass; s++) {
                out << '\t' << l.slots[s] << '\t' << l.names[s];
            }
            out << '\n';
        }
        if (!out) {
            return false;
        }
    }
    return MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void SetStatus(bool isError, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_status = buf;
    g_statusIsError = isError;
    g_statusUntil = ImGui::GetTime() + kStatusSeconds;
}

void Persist() {
    if (!WriteSavedLoadouts()) {
        SetStatus(true, "Couldn't write %s. Your changes are only in memory.", kLoadoutsFile);
        LogF("loadouts: no pude escribir %s", kLoadoutsFile);
    }
}

void SaveAsNew(int classId, const int slots[kSlotsPerClass], const char *rawTitle) {
    SavedLoadout l;
    l.classId = classId;
    l.title = CleanTitle(rawTitle);
    if (l.title.empty()) {
        l.title = "Untitled";
    }
    for (int s = 0; s < kSlotsPerClass; s++) {
        l.slots[s] = slots[s];
        const Weapon *w = ArmaPorIndice(slots[s]);
        l.names[s] = w ? w->name : std::string();
    }
    MarkOutdated(l);
    g_loadouts.push_back(l);
    Persist();
    SetStatus(false, "Saved \"%s\".", l.title.c_str());
}

void Apply(const SavedLoadout &l, int activeClass) {
    uint32_t *table = g_table.load();
    if (l.outdated || !SlotsFitClass(l.classId, l.slots)) {
        SetStatus(true, "\"%s\" doesn't match the weapon list anymore. Delete it and save it again.",
                  l.title.c_str());
        return;
    }
    if (!table || !WriteIfValid(table, l.classId, l.slots)) {
        SetStatus(true, "Equipment isn't reachable right now. Try again from the lobby.");
        Log("loadouts: no pude escribir el loadout: la tabla no esta o dejo de ser valida");
        return;
    }
    LogF("loadouts: cargado \"%s\" en %s", l.title.c_str(), kClassNames[l.classId]);
    if (l.classId != activeClass) {
        SetStatus(false,
                  "Loaded \"%s\" and switched to %s. In the lobby, open Class/Equipment to see it; "
                  "in a mission it applies from the next one.",
                  l.title.c_str(), kClassNames[l.classId]);
    } else {
        SetStatus(false, "Loaded \"%s\". In a mission it applies from the next one.", l.title.c_str());
    }
}

int SlotsThatDiffer(const int a[kSlotsPerClass], const int b[kSlotsPerClass]) {
    int n = 0;
    for (int s = 0; s < kSlotsPerClass; s++) {
        n += a[s] != b[s] ? 1 : 0;
    }
    return n;
}

constexpr float kWindowW = 1060.0f;
constexpr float kWindowH = 740.0f;
constexpr float kHeaderH = 58.0f;
constexpr float kSidebarW = 320.0f;
constexpr float kFooterH = 92.0f;
constexpr float kItemH = 56.0f;
constexpr float kRowH = 45.0f;
constexpr float kDesignScale = 0.8f;

constexpr uint32_t kBg = 0x0B0E0C;
constexpr uint32_t kLine = 0x242A26;
constexpr uint32_t kSelected = 0x1A201C;
constexpr uint32_t kText = 0xE8ECE9;
constexpr uint32_t kSoft = 0xB7BEBA;
constexpr uint32_t kMuted = 0x8C938F;
constexpr uint32_t kFaint = 0x5A615D;
constexpr uint32_t kGreen = 0x5ED17A;
constexpr uint32_t kOnGreen = 0x0B1A10;
constexpr uint32_t kAmber = 0xF0A73A;
constexpr uint32_t kDiffRow = 0x1B1A13;
constexpr uint32_t kRowLine = 0x1D221F;
constexpr uint32_t kKeyLine = 0x3A413C;
constexpr uint32_t kDanger = 0xFF8C73;
constexpr uint32_t kMaxed = 0xFAC775;

enum class Mode { Browse, Naming, Renaming, ConfirmDelete };

struct PanelState {
    int viewedClass = 0;
    int selected[kClassCount] = {-1, -1, -1, -1};
    Mode mode = Mode::Browse;
    char text[64] = "";
    bool focusPending = false;
    bool scrollPending = false;
    bool pickActiveTab = true;
};

PanelState g_ui;
float g_k = 1.0f;
ImFont *g_fontLabel = nullptr;
ImFont *g_fontBold = nullptr;
ImFont *g_fontSemi = nullptr;

ImU32 Rgb(uint32_t hex, float alpha = 1.0f) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, (int)(alpha * 255.0f));
}

float D(float px) {
    return px * g_k;
}

ImFont *FontOr(ImFont *f) {
    return f ? f : ImGui::GetFont();
}

ImVec2 Measure(ImFont *f, float px, const char *t, const char *end = nullptr) {
    return FontOr(f)->CalcTextSizeA(D(px), FLT_MAX, 0.0f, t, end);
}

void PaintText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const char *t) {
    dl->AddText(FontOr(f), D(px), p, Rgb(col), t);
}

int Utf8Length(unsigned char c) {
    return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
}

float SpacedText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const char *t, float spacing) {
    const float start = p.x;
    for (const char *c = t; *c;) {
        const int n = Utf8Length((unsigned char)*c);
        dl->AddText(FontOr(f), D(px), p, Rgb(col), c, c + n);
        p.x += Measure(f, px, c, c + n).x + D(spacing);
        c += n;
    }
    return p.x - start;
}

std::string Upper(const char *s) {
    std::string out = s;
    for (char &ch : out) {
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
    }
    return out;
}

std::string FitText(ImFont *f, float px, const std::string &s, float maxWidth, bool &cut) {
    cut = Measure(f, px, s.c_str()).x > maxWidth;
    if (!cut) {
        return s;
    }
    const std::string ellipsis = "\xE2\x80\xA6";
    std::string t = s;
    while (!t.empty()) {
        size_t at = t.size() - 1;
        while (at > 0 && ((unsigned char)t[at] & 0xC0) == 0x80) {
            at--;
        }
        t.erase(at);
        if (Measure(f, px, (t + ellipsis).c_str()).x <= maxWidth) {
            break;
        }
    }
    return t + ellipsis;
}

void FittedText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const std::string &s, float maxWidth) {
    bool cut = false;
    const std::string shown = FitText(f, px, s, maxWidth, cut);
    PaintText(dl, f, px, p, col, shown.c_str());
    const ImVec2 size = Measure(f, px, shown.c_str());
    if (cut && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + size.x, p.y + size.y))) {
        ImGui::SetTooltip("%s", s.c_str());
    }
}

float KeyHint(ImDrawList *dl, ImVec2 p, const char *key, uint32_t text, uint32_t border) {
    const ImVec2 size = Measure(g_fontLabel, 12.0f, key);
    const float w = size.x + D(12.0f);
    const float h = D(22.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), Rgb(border), 0.0f, D(1.0f));
    PaintText(dl, g_fontLabel, 12.0f, ImVec2(p.x + D(6.0f), p.y + (h - size.y) * 0.5f), text, key);
    return w;
}

void DashedRect(ImDrawList *dl, ImVec2 a, ImVec2 b, uint32_t col) {
    const float dash = D(5.0f);
    const float gap = D(4.0f);
    const float t = D(1.0f);
    for (float x = a.x; x < b.x; x += dash + gap) {
        const float x2 = x + dash < b.x ? x + dash : b.x;
        dl->AddLine(ImVec2(x, a.y), ImVec2(x2, a.y), Rgb(col), t);
        dl->AddLine(ImVec2(x, b.y), ImVec2(x2, b.y), Rgb(col), t);
    }
    for (float y = a.y; y < b.y; y += dash + gap) {
        const float y2 = y + dash < b.y ? y + dash : b.y;
        dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, y2), Rgb(col), t);
        dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, y2), Rgb(col), t);
    }
}

enum class ButtonKind { Primary, Normal, Danger };

bool ActionButton(ImDrawList *dl, const char *id, ImVec2 p, const char *label, const char *key, ButtonKind kind,
                  float &width) {
    const float h = D(48.0f);
    const ImVec2 labelSize = Measure(g_fontSemi, 16.0f, label);
    const float keyWidth = Measure(g_fontLabel, 12.0f, key).x + D(12.0f);
    width = D(20.0f) + labelSize.x + D(12.0f) + keyWidth + D(20.0f);
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 q(p.x + width, p.y + h);
    uint32_t textColor = kText;
    uint32_t keyText = kMuted;
    uint32_t keyBorder = kKeyLine;
    if (kind == ButtonKind::Primary) {
        dl->AddRectFilled(p, q, Rgb(kGreen, hovered ? 1.0f : 0.88f));
        textColor = kOnGreen;
        keyText = kOnGreen;
        keyBorder = kOnGreen;
    } else {
        if (hovered) {
            dl->AddRectFilled(p, q, Rgb(kSelected));
        }
        const uint32_t border = kind == ButtonKind::Danger ? kDanger : kKeyLine;
        dl->AddRect(p, q, Rgb(border), 0.0f, D(1.0f));
        if (kind == ButtonKind::Danger) {
            textColor = kDanger;
        }
    }
    PaintText(dl, g_fontSemi, 16.0f, ImVec2(p.x + D(20.0f), p.y + (h - labelSize.y) * 0.5f), textColor, label);
    KeyHint(dl, ImVec2(p.x + D(20.0f) + labelSize.x + D(12.0f), p.y + D(13.0f)), key, keyText, keyBorder);
    return clicked;
}

float FontBase(float px) {
    return D(px) / ImGui::GetStyle().FontScaleMain;
}

bool TitleInput(ImVec2 p, float width, float px, const char *hint) {
    ImGui::SetCursorScreenPos(p);
    ImGui::SetNextItemWidth(width);
    ImGui::PushFont(g_fontSemi, FontBase(px));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgb(kSelected));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgb(kText));
    ImGui::PushStyleColor(ImGuiCol_Border, Rgb(kGreen));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, D(1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(D(10.0f), D(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    if (g_ui.focusPending) {
        ImGui::SetKeyboardFocusHere();
        g_ui.focusPending = false;
    }
    const bool enter = ImGui::InputTextWithHint("##title", hint, g_ui.text, sizeof(g_ui.text),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    return enter;
}

std::vector<int> LoadoutsOfClass(int classId) {
    std::vector<int> out;
    for (int i = 0; i < (int)g_loadouts.size(); i++) {
        if (g_loadouts[i].classId == classId) {
            out.push_back(i);
        }
    }
    return out;
}

int SelectedFor(int classId) {
    const int i = g_ui.selected[classId];
    if (i >= 0 && i < (int)g_loadouts.size() && g_loadouts[i].classId == classId) {
        return i;
    }
    const std::vector<int> mine = LoadoutsOfClass(classId);
    g_ui.selected[classId] = mine.empty() ? -1 : mine[0];
    return g_ui.selected[classId];
}

void StartTyping(Mode mode, const char *initial) {
    g_ui.mode = mode;
    strncpy_s(g_ui.text, initial, _TRUNCATE);
    g_ui.focusPending = true;
}

void CommitTyping(const Equipment &e) {
    const int c = g_ui.viewedClass;
    if (g_ui.mode == Mode::Naming) {
        SaveAsNew(c, e.slots[c], g_ui.text);
        g_ui.selected[c] = (int)g_loadouts.size() - 1;
        g_ui.scrollPending = true;
    } else if (g_ui.mode == Mode::Renaming) {
        const int sel = SelectedFor(c);
        const std::string title = CleanTitle(g_ui.text);
        if (sel >= 0 && !title.empty()) {
            g_loadouts[sel].title = title;
            Persist();
            SetStatus(false, "Renamed to \"%s\".", title.c_str());
        }
    }
    g_ui.mode = Mode::Browse;
}

void DeleteSelected() {
    const int sel = SelectedFor(g_ui.viewedClass);
    if (sel < 0) {
        return;
    }
    const std::string title = g_loadouts[sel].title;
    LogF("loadouts: borrado \"%s\"", title.c_str());
    g_loadouts.erase(g_loadouts.begin() + sel);
    for (int &s : g_ui.selected) {
        s = -1;
    }
    Persist();
    SetStatus(false, "Deleted \"%s\".", title.c_str());
    g_ui.mode = Mode::Browse;
}

void SwitchTab(int classId) {
    g_ui.viewedClass = (classId + kClassCount) % kClassCount;
    g_ui.mode = Mode::Browse;
    g_ui.scrollPending = true;
}

void HandleKeys(const Equipment &e) {
    const bool typing = g_ui.mode == Mode::Naming || g_ui.mode == Mode::Renaming;
    if (typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_ui.mode = Mode::Browse;
        }
        return;
    }
    const int sel = SelectedFor(g_ui.viewedClass);
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if (g_ui.mode == Mode::ConfirmDelete) {
        if (enter) {
            DeleteSelected();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_ui.mode = Mode::Browse;
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        SwitchTab(g_ui.viewedClass - 1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        SwitchTab(g_ui.viewedClass + 1);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        StartTyping(Mode::Naming, "");
    } else if (sel >= 0 && enter) {
        Apply(g_loadouts[sel], e.activeClass);
    } else if (sel >= 0 && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        StartTyping(Mode::Renaming, g_loadouts[sel].title.c_str());
    } else if (sel >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        g_ui.mode = Mode::ConfirmDelete;
    } else if (sel >= 0 && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
        const std::vector<int> mine = LoadoutsOfClass(g_ui.viewedClass);
        int pos = 0;
        while (pos < (int)mine.size() && mine[pos] != sel) {
            pos++;
        }
        pos += ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 1;
        if (pos >= 0 && pos < (int)mine.size()) {
            g_ui.selected[g_ui.viewedClass] = mine[pos];
            g_ui.scrollPending = true;
        }
    }
}

std::string KeyName(int vk) {
    char buf[16];
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
    } else {
        snprintf(buf, sizeof(buf), "0x%02X", vk);
    }
    return buf;
}

void DrawHeader(ImDrawList *dl, ImVec2 o, float w, const Equipment *e, bool &open) {
    const float h = D(kHeaderH);
    dl->AddRectFilled(ImVec2(o.x + D(20.0f), o.y + D(25.0f)), ImVec2(o.x + D(28.0f), o.y + D(33.0f)), Rgb(kGreen));
    SpacedText(dl, g_fontBold, 16.0f, ImVec2(o.x + D(38.0f), o.y + D(18.0f)), kText, "LOADOUTS", 2.5f);
    dl->AddLine(ImVec2(o.x + D(172.0f), o.y), ImVec2(o.x + D(172.0f), o.y + h), Rgb(kLine), D(1.0f));

    if (e) {
        float x = o.x + D(186.0f);
        x += KeyHint(dl, ImVec2(x, o.y + D(18.0f)), "Q", kMuted, kKeyLine) + D(10.0f);
        for (int c = 0; c < kClassCount; c++) {
            char count[8];
            snprintf(count, sizeof(count), "%d", (int)LoadoutsOfClass(c).size());
            const ImVec2 nameSize = Measure(g_fontLabel, 16.0f, kClassNames[c]);
            const ImVec2 countSize = Measure(g_fontLabel, 12.0f, count);
            const bool inUse = c == e->activeClass;
            const float pillW = inUse ? Measure(g_fontLabel, 10.0f, "IN USE").x + D(1.0f) * 6 + D(12.0f) : 0.0f;
            const float tabW = D(16.0f) + nameSize.x + D(7.0f) + countSize.x + (inUse ? D(8.0f) + pillW : 0.0f) + D(16.0f);

            ImGui::SetCursorScreenPos(ImVec2(x, o.y));
            ImGui::PushID(c);
            if (ImGui::InvisibleButton("tab", ImVec2(tabW, h))) {
                SwitchTab(c);
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const bool viewed = c == g_ui.viewedClass;
            const float textY = o.y + (h - nameSize.y) * 0.5f;
            PaintText(dl, g_fontLabel, 16.0f, ImVec2(x + D(16.0f), textY), viewed || hovered ? kText : kMuted,
                     kClassNames[c]);
            const float countX = x + D(16.0f) + nameSize.x + D(7.0f);
            PaintText(dl, g_fontLabel, 12.0f, ImVec2(countX, textY + nameSize.y - countSize.y - D(2.0f)), kFaint, count);
            if (inUse) {
                const float pillX = countX + countSize.x + D(8.0f);
                const ImVec2 a(pillX, o.y + D(20.0f));
                const ImVec2 b(pillX + pillW, o.y + D(38.0f));
                dl->AddRectFilled(a, b, Rgb(kGreen));
                const float labelH = Measure(g_fontLabel, 10.0f, "IN USE").y;
                SpacedText(dl, g_fontLabel, 10.0f, ImVec2(a.x + D(6.0f), a.y + (b.y - a.y - labelH) * 0.5f),
                           kOnGreen, "IN USE", 1.0f);
            }
            if (viewed) {
                dl->AddRectFilled(ImVec2(x, o.y + h - D(2.0f)), ImVec2(x + tabW, o.y + h), Rgb(kGreen));
            }
            x += tabW;
        }
        KeyHint(dl, ImVec2(x + D(10.0f), o.y + D(18.0f)), "E", kMuted, kKeyLine);
    }

    const std::string key = KeyName(TeclaLoadouts());
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

    dl->AddLine(ImVec2(o.x, o.y + h), ImVec2(o.x + w, o.y + h), Rgb(kLine), D(1.0f));
}

void DrawSidebar(ImDrawList *dl, ImVec2 o, float bodyTop, float footerTop, const Equipment &e) {
    const int c = g_ui.viewedClass;
    const std::string label = "SAVED \xC2\xB7 " + Upper(kClassNames[c]);
    SpacedText(dl, g_fontLabel, 12.0f, ImVec2(o.x + D(20.0f), bodyTop + D(18.0f)), kFaint, label.c_str(), 1.5f);

    const float listTop = bodyTop + D(44.0f);
    ImGui::SetCursorScreenPos(ImVec2(o.x, listTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("list", ImVec2(D(kSidebarW) - D(1.0f), footerTop - listTop), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList *ldl = ImGui::GetWindowDrawList();
    const int sel = SelectedFor(c);
    for (int i : LoadoutsOfClass(c)) {
        const SavedLoadout &l = g_loadouts[i];
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float itemW = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("item", ImVec2(itemW, D(kItemH)))) {
            g_ui.selected[c] = i;
            g_ui.mode = Mode::Browse;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool isSelected = i == sel;
        if (isSelected && g_ui.scrollPending) {
            ImGui::SetScrollHereY(0.5f);
            g_ui.scrollPending = false;
        }
        if (isSelected || hovered) {
            ldl->AddRectFilled(p, ImVec2(p.x + itemW, p.y + D(kItemH)), Rgb(kSelected, isSelected ? 1.0f : 0.5f));
        }
        const ImVec2 bullet(p.x + D(22.0f), p.y + D(18.0f));
        ldl->AddRectFilled(bullet, ImVec2(bullet.x + D(6.0f), bullet.y + D(6.0f)), Rgb(isSelected ? kText : kFaint));
        FittedText(ldl, g_fontBold, 16.0f, ImVec2(p.x + D(38.0f), p.y + D(9.0f)), kText, l.title,
                   itemW - D(38.0f) - D(14.0f));
        const int differ = SlotsThatDiffer(l.slots, e.slots[c]);
        char sub[48];
        if (l.outdated) {
            snprintf(sub, sizeof(sub), "Weapon list changed");
        } else if (differ == 0) {
            snprintf(sub, sizeof(sub), "Equipped");
        } else {
            snprintf(sub, sizeof(sub), "%d of %d slots differ", differ, kSlotsPerClass);
        }
        PaintText(ldl, nullptr, 12.5f, ImVec2(p.x + D(38.0f), p.y + D(31.0f)),
                 l.outdated ? kDanger : (differ == 0 ? kGreen : kMuted), sub);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    const ImVec2 a(o.x + D(16.0f), footerTop + D(20.0f));
    const ImVec2 b(o.x + D(kSidebarW) - D(16.0f), footerTop + D(72.0f));
    if (g_ui.mode == Mode::Naming) {
        if (TitleInput(ImVec2(a.x, a.y + D(6.0f)), b.x - a.x, 14.0f, "Loadout name")) {
            CommitTyping(e);
        }
        return;
    }
    ImGui::SetCursorScreenPos(a);
    if (ImGui::InvisibleButton("savenew", ImVec2(b.x - a.x, b.y - a.y))) {
        StartTyping(Mode::Naming, "");
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        dl->AddRectFilled(a, b, Rgb(kSelected));
    }
    DashedRect(dl, a, b, hovered ? kMuted : kKeyLine);
    const ImVec2 labelSize = Measure(g_fontSemi, 14.0f, "+ Save equipped as new");
    PaintText(dl, g_fontSemi, 14.0f, ImVec2(a.x + D(14.0f), a.y + (b.y - a.y - labelSize.y) * 0.5f), kText,
             "+ Save equipped as new");
    const ImVec2 shortcutSize = Measure(g_fontLabel, 12.0f, "Ctrl S");
    PaintText(dl, g_fontLabel, 12.0f, ImVec2(b.x - D(14.0f) - shortcutSize.x, a.y + (b.y - a.y - shortcutSize.y) * 0.5f),
             kFaint, "Ctrl S");
}

void DrawDetail(ImDrawList *dl, float px, float pw, float bodyTop, float footerTop, const Equipment &e) {
    const int c = g_ui.viewedClass;
    const int sel = SelectedFor(c);
    if (sel < 0) {
        char title[96];
        snprintf(title, sizeof(title), "No saved loadouts for %s yet", kClassNames[c]);
        const char *hint = "Equip what you want in the game, then save it with Ctrl S.";
        const ImVec2 titleSize = Measure(g_fontBold, 20.0f, title);
        const ImVec2 hintSize = Measure(nullptr, 14.0f, hint);
        const float cy = bodyTop + (footerTop - bodyTop) * 0.5f;
        PaintText(dl, g_fontBold, 20.0f, ImVec2(px + (pw - titleSize.x) * 0.5f, cy - titleSize.y), kText, title);
        PaintText(dl, nullptr, 14.0f, ImVec2(px + (pw - hintSize.x) * 0.5f, cy + D(6.0f)), kMuted, hint);
        return;
    }

    const SavedLoadout &l = g_loadouts[sel];
    const int differ = SlotsThatDiffer(l.slots, e.slots[c]);
    char label[64];
    uint32_t labelColor = kMuted;
    if (l.outdated) {
        snprintf(label, sizeof(label), "WEAPON LIST CHANGED \xC2\xB7 SAVE IT AGAIN");
        labelColor = kDanger;
    } else if (differ == 0) {
        snprintf(label, sizeof(label), "MATCHES EQUIPPED");
        labelColor = kGreen;
    } else {
        snprintf(label, sizeof(label), "%d CHANGE%s FROM EQUIPPED", differ, differ == 1 ? "" : "S");
    }
    SpacedText(dl, g_fontLabel, 12.0f, ImVec2(px + D(28.0f), bodyTop + D(20.0f)), labelColor, label, 1.5f);

    if (g_ui.mode == Mode::Renaming) {
        if (TitleInput(ImVec2(px + D(24.0f), bodyTop + D(40.0f)), pw - D(64.0f), 22.0f, "Loadout name")) {
            CommitTyping(e);
        }
    } else {
        FittedText(dl, g_fontBold, 26.0f, ImVec2(px + D(28.0f), bodyTop + D(40.0f)), kText, l.title, pw - D(64.0f));
    }

    const float tl = px + D(18.0f);
    const float tr = px + pw - D(36.0f);
    const float headerY = bodyTop + D(100.0f);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(tl + D(10.0f), headerY), kFaint, "SLOT", 1.2f);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(tl + D(70.0f), headerY), kFaint, "EQUIPPED NOW", 1.2f);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(tl + D(335.0f), headerY), kFaint, "THIS LOADOUT", 1.2f);
    const float lvHeaderW = Measure(g_fontLabel, 11.0f, "LV").x + D(1.2f);
    SpacedText(dl, g_fontLabel, 11.0f, ImVec2(tr - D(10.0f) - lvHeaderW, headerY), kFaint, "LV", 1.2f);

    const float rowsTop = bodyTop + D(122.0f);
    const float equippedWidth = D(250.0f);
    const float loadoutX = tl + D(335.0f);
    const float loadoutWidth = tr - D(70.0f) - loadoutX;
    for (int s = 0; s < kSlotsPerClass; s++) {
        const float y = rowsTop + D(kRowH) * s;
        const bool diff = l.slots[s] != e.slots[c][s];
        if (diff) {
            dl->AddRectFilled(ImVec2(tl, y), ImVec2(tr, y + D(kRowH)), Rgb(kDiffRow));
        }
        if (s == 0 || s == kWeaponSlotsPerClass) {
            dl->AddLine(ImVec2(tl, y), ImVec2(tr, y), Rgb(s == 0 ? kRowLine : kKeyLine), D(1.0f));
        }
        dl->AddLine(ImVec2(tl, y + D(kRowH)), ImVec2(tr, y + D(kRowH)), Rgb(kRowLine), D(1.0f));

        const float textH = Measure(nullptr, 16.0f, "Ag").y;
        const float ty = y + (D(kRowH) - textH) * 0.5f;
        PaintText(dl, g_fontLabel, 13.0f, ImVec2(tl + D(10.0f), ty + D(1.0f)), kFaint, kSlotLabels[s]);

        const Weapon *now = ArmaPorIndice(e.slots[c][s]);
        FittedText(dl, nullptr, 16.0f, ImVec2(tl + D(70.0f), ty), diff ? kSoft : kMuted,
                   now ? now->name : std::string("?"), equippedWidth);

        const Weapon *mine = ArmaPorIndice(l.slots[s]);
        const std::string mineName = mine ? mine->name : l.names[s];
        if (diff) {
            const float my = y + D(kRowH) * 0.5f - D(3.0f);
            dl->AddRectFilled(ImVec2(loadoutX - D(13.0f), my), ImVec2(loadoutX - D(7.0f), my + D(6.0f)), Rgb(kAmber));
        }
        FittedText(dl, diff ? g_fontSemi : nullptr, 16.0f, ImVec2(loadoutX, ty), diff ? kText : kSoft, mineName,
                   loadoutWidth);

        if (mine) {
            char level[16];
            snprintf(level, sizeof(level), "Lv%d", mine->level);
            const ImVec2 lvSize = Measure(g_fontLabel, 14.0f, level);
            PaintText(dl, g_fontLabel, 14.0f, ImVec2(tr - D(10.0f) - lvSize.x, y + (D(kRowH) - lvSize.y) * 0.5f),
                     mine->starred ? kMaxed : kMuted, level);
        }
    }
}

void DrawFooter(ImDrawList *dl, float px, float pw, float footerTop, const Equipment &e) {
    dl->AddLine(ImVec2(px, footerTop), ImVec2(px + pw, footerTop), Rgb(kLine), D(1.0f));
    const int sel = SelectedFor(g_ui.viewedClass);
    float x = px + D(28.0f);
    const float y = footerTop + D(22.0f);
    float w = 0.0f;

    if (g_ui.mode == Mode::Naming || g_ui.mode == Mode::Renaming) {
        if (ActionButton(dl, "save", ImVec2(x, y), "Save", "Enter", ButtonKind::Primary, w)) {
            CommitTyping(e);
        }
        x += w + D(10.0f);
        if (ActionButton(dl, "cancel", ImVec2(x, y), "Cancel", "Esc", ButtonKind::Normal, w)) {
            g_ui.mode = Mode::Browse;
        }
        x += w;
    } else if (g_ui.mode == Mode::ConfirmDelete && sel >= 0) {
        if (ActionButton(dl, "confirm", ImVec2(x, y), "Delete", "Enter", ButtonKind::Danger, w)) {
            DeleteSelected();
        }
        x += w + D(10.0f);
        if (ActionButton(dl, "keep", ImVec2(x, y), "Keep", "Esc", ButtonKind::Normal, w)) {
            g_ui.mode = Mode::Browse;
        }
        x += w + D(16.0f);
        char question[96];
        snprintf(question, sizeof(question), "Delete \"%s\"?", g_loadouts[sel].title.c_str());
        const ImVec2 qs = Measure(g_fontSemi, 14.0f, question);
        PaintText(dl, g_fontSemi, 14.0f, ImVec2(x, y + (D(48.0f) - qs.y) * 0.5f), kDanger, question);
        return;
    } else if (sel >= 0) {
        if (ActionButton(dl, "load", ImVec2(x, y), "Load loadout", "Enter", ButtonKind::Primary, w)) {
            Apply(g_loadouts[sel], e.activeClass);
        }
        x += w + D(10.0f);
        if (ActionButton(dl, "rename", ImVec2(x, y), "Rename", "R", ButtonKind::Normal, w)) {
            StartTyping(Mode::Renaming, g_loadouts[sel].title.c_str());
        }
        x += w + D(10.0f);
        if (ActionButton(dl, "delete", ImVec2(x, y), "Delete", "Del", ButtonKind::Normal, w)) {
            g_ui.mode = Mode::ConfirmDelete;
        }
        x += w;
    }

    if (!g_status.empty() && ImGui::GetTime() < g_statusUntil) {
        const float sx = x + D(20.0f);
        const float wrap = px + pw - D(24.0f) - sx;
        if (wrap > D(80.0f)) {
            dl->AddText(FontOr(nullptr), D(13.0f), ImVec2(sx, footerTop + D(18.0f)),
                        Rgb(g_statusIsError ? kDanger : kGreen), g_status.c_str(), nullptr, wrap);
        }
    }
}

}

bool CurrentEquipment(Equipment &out) {
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    if (g_snapshotValid) {
        out = g_snapshot;
    }
    return g_snapshotValid;
}

void LoadLoadoutsFonts() {
    struct {
        const char *path;
        ImFont **target;
    } fonts[] = {
        {"C:\\Windows\\Fonts\\bahnschrift.ttf", &g_fontLabel},
        {"C:\\Windows\\Fonts\\segoeuib.ttf", &g_fontBold},
        {"C:\\Windows\\Fonts\\seguisb.ttf", &g_fontSemi},
    };
    ImGuiIO &io = ImGui::GetIO();
    for (auto &f : fonts) {
        if (GetFileAttributesA(f.path) == INVALID_FILE_ATTRIBUTES) {
            LogF("loadouts: no esta %s, uso la fuente por defecto", f.path);
            continue;
        }
        *f.target = io.Fonts->AddFontFromFileTTF(f.path, 16.0f);
    }
}

void DrawLoadoutsPanel(bool &open, float scale) {
    static int lastFrame = -2;
    if (ImGui::GetFrameCount() != lastFrame + 1) {
        g_ui.pickActiveTab = true;
        g_ui.mode = Mode::Browse;
    }
    lastFrame = ImGui::GetFrameCount();

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    g_k = scale * kDesignScale;
    if (kWindowW * g_k > screen.x * 0.95f) {
        g_k = screen.x * 0.95f / kWindowW;
    }
    if (kWindowH * g_k > screen.y * 0.92f) {
        g_k = screen.y * 0.92f / kWindowH;
    }
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
    const bool visible = ImGui::Begin("##loadouts", &open, flags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (!visible) {
        ImGui::End();
        return;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float bodyTop = o.y + D(kHeaderH);
    const float footerTop = o.y + size.y - D(kFooterH);

    Equipment e;
    if (!CurrentEquipment(e)) {
        DrawHeader(dl, o, size.x, nullptr, open);
        const char *title = "Equipment not found yet";
        const char *hint = "Load your save and go to the lobby; it's picked up within a few seconds.";
        const ImVec2 ts = Measure(g_fontBold, 20.0f, title);
        const ImVec2 hs = Measure(nullptr, 14.0f, hint);
        const float cy = bodyTop + (o.y + size.y - bodyTop) * 0.5f;
        PaintText(dl, g_fontBold, 20.0f, ImVec2(o.x + (size.x - ts.x) * 0.5f, cy - ts.y), kText, title);
        PaintText(dl, nullptr, 14.0f, ImVec2(o.x + (size.x - hs.x) * 0.5f, cy + D(6.0f)), kMuted, hint);
        ImGui::End();
        return;
    }

    if (g_ui.pickActiveTab) {
        g_ui.viewedClass = e.activeClass;
        g_ui.pickActiveTab = false;
        g_ui.scrollPending = true;
    }

    HandleKeys(e);
    DrawHeader(dl, o, size.x, &e, open);
    dl->AddLine(ImVec2(o.x + D(kSidebarW), bodyTop), ImVec2(o.x + D(kSidebarW), o.y + size.y), Rgb(kLine), D(1.0f));
    DrawSidebar(dl, o, bodyTop, footerTop, e);
    const float px = o.x + D(kSidebarW);
    const float pw = size.x - D(kSidebarW);
    DrawDetail(dl, px, pw, bodyTop, footerTop, e);
    DrawFooter(dl, px, pw, footerTop, e);
    ImGui::End();
}

void InitLoadouts() {
    int maxIndex = -1;
    for (const ClassData &c : GetCatalog().classes) {
        for (const Weapon &w : c.weapons) {
            maxIndex = w.index > maxIndex ? w.index : maxIndex;
        }
    }
    g_classOfWeapon.assign(maxIndex + 1, -1);
    for (const ClassData &c : GetCatalog().classes) {
        const int classId = ClassIdByName(c.name);
        for (const Weapon &w : c.weapons) {
            g_classOfWeapon[w.index] = classId;
        }
    }
    if (g_classOfWeapon.empty()) {
        Log("loadouts: sin catalogo, no busco la tabla");
        return;
    }
    LoadSavedLoadouts();
    LogF("loadouts: catalogo de %d armas; arranca la busqueda de la tabla", (int)g_classOfWeapon.size());
    CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
}
