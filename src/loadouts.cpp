#include <windows.h>

#include <atomic>
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
constexpr float kCardWidth = 430.0f;
constexpr int kCardsPerRow = 2;
constexpr const char *kLoadoutsFile = "Mods\\Compendium\\loadouts.tsv";

const char *const kClassNames[kClassCount] = {"Ranger", "Wing Diver", "Air Raider", "Fencer"};

const ImVec4 kCardHeader(0.06f, 0.43f, 0.34f, 0.95f);
const ImVec4 kCardHeaderEquipped(0.11f, 0.62f, 0.46f, 0.95f);
const ImVec4 kCardRow(0.03f, 0.31f, 0.25f, 0.90f);
const ImVec4 kCardGap(0.02f, 0.20f, 0.17f, 0.90f);
const ImVec4 kCardBorder(0.36f, 0.79f, 0.65f, 1.0f);
const ImVec4 kLevel(0.62f, 0.88f, 0.80f, 1.0f);
const ImVec4 kLevelMaxed(0.98f, 0.78f, 0.46f, 1.0f);
const ImVec4 kWarning(1.0f, 0.55f, 0.45f, 1.0f);

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

bool SameSlots(const int a[kSlotsPerClass], const int b[kSlotsPerClass]) {
    return memcmp(a, b, sizeof(int) * kSlotsPerClass) == 0;
}

void RightAlignedText(const ImVec4 &color, const char *text) {
    const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(x);
    ImGui::TextColored(color, "%s", text);
}

void DrawCardTable(const char *title, const char *tag, bool highlighted, const int slots[kSlotsPerClass],
                   float scale) {
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, kCardBorder);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kCardGap);
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable("card", 2, flags, ImVec2(kCardWidth * scale, 0.0f))) {
        ImGui::TableSetupColumn("weapon", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("level", ImGuiTableColumnFlags_WidthFixed, 90.0f * scale);

        ImGui::TableNextRow();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                               ImGui::GetColorU32(highlighted ? kCardHeaderEquipped : kCardHeader));
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(title);
        if (tag) {
            ImGui::TableSetColumnIndex(1);
            RightAlignedText(kLevel, tag);
        }

        for (int s = 0; s < kSlotsPerClass; s++) {
            if (s == kWeaponSlotsPerClass) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 6.0f * scale);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(kCardGap));
            }
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(kCardRow));
            const Weapon *w = ArmaPorIndice(slots[s]);
            ImGui::TableSetColumnIndex(0);
            if (w) {
                ImGui::TextUnformatted(w->name.c_str());
            } else {
                ImGui::TextDisabled("unknown weapon #%d", slots[s]);
            }
            ImGui::TableSetColumnIndex(1);
            if (w) {
                char level[16];
                snprintf(level, sizeof(level), "Lv%d", w->level);
                RightAlignedText(w->starred ? kLevelMaxed : kLevel, level);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(2);
}

struct PanelState {
    int namingClass = -1;
    char newTitle[64] = "";
    int renaming = -1;
    char renameTitle[64] = "";
    int confirmingDelete = -1;
    bool focusPending = false;
};

PanelState g_ui;

void FocusIfPending() {
    if (g_ui.focusPending) {
        ImGui::SetKeyboardFocusHere();
        g_ui.focusPending = false;
    }
}

void DrawEquippedCard(int classId, const Equipment &e, float scale) {
    ImGui::BeginGroup();
    DrawCardTable("Equipped now", "live", true, e.slots[classId], scale);
    if (g_ui.namingClass == classId) {
        ImGui::SetNextItemWidth(kCardWidth * scale * 0.55f);
        FocusIfPending();
        const bool enter = ImGui::InputTextWithHint("##newtitle", "Loadout name", g_ui.newTitle,
                                                    sizeof(g_ui.newTitle), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Save") || enter) {
            SaveAsNew(classId, e.slots[classId], g_ui.newTitle);
            g_ui.namingClass = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            g_ui.namingClass = -1;
        }
    } else if (ImGui::Button("Save as new")) {
        g_ui.namingClass = classId;
        g_ui.renaming = -1;
        g_ui.confirmingDelete = -1;
        g_ui.newTitle[0] = '\0';
        g_ui.focusPending = true;
    }
    ImGui::EndGroup();
}

void DrawSavedCard(int index, const Equipment &e, float scale) {
    SavedLoadout &l = g_loadouts[index];
    const bool equipped = SameSlots(l.slots, e.slots[l.classId]);

    ImGui::PushID(index);
    ImGui::BeginGroup();
    DrawCardTable(l.title.c_str(), equipped ? "equipped" : nullptr, equipped, l.slots, scale);

    if (l.outdated) {
        ImGui::TextColored(kWarning, "Weapon list changed since this was saved.");
    }

    if (g_ui.renaming == index) {
        ImGui::SetNextItemWidth(kCardWidth * scale * 0.55f);
        FocusIfPending();
        const bool enter = ImGui::InputText("##rename", g_ui.renameTitle, sizeof(g_ui.renameTitle),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("OK") || enter) {
            const std::string title = CleanTitle(g_ui.renameTitle);
            if (!title.empty()) {
                l.title = title;
                Persist();
            }
            g_ui.renaming = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            g_ui.renaming = -1;
        }
    } else if (g_ui.confirmingDelete == index) {
        ImGui::TextColored(kWarning, "Delete \"%s\"?", l.title.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            LogF("loadouts: borrado \"%s\"", l.title.c_str());
            g_loadouts.erase(g_loadouts.begin() + index);
            g_ui.confirmingDelete = -1;
            Persist();
            ImGui::EndGroup();
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep")) {
            g_ui.confirmingDelete = -1;
        }
    } else {
        if (ImGui::Button("Load")) {
            Apply(l, e.activeClass);
        }
        ImGui::SameLine();
        if (ImGui::Button("Rename")) {
            g_ui.renaming = index;
            g_ui.confirmingDelete = -1;
            g_ui.namingClass = -1;
            g_ui.focusPending = true;
            strncpy_s(g_ui.renameTitle, l.title.c_str(), _TRUNCATE);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            g_ui.confirmingDelete = index;
            g_ui.renaming = -1;
            g_ui.namingClass = -1;
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void DrawClassTab(int classId, const Equipment &e, float scale) {
    ImGui::BeginChild("cards");
    DrawEquippedCard(classId, e, scale);
    int drawn = 1;
    for (int i = 0; i < (int)g_loadouts.size(); i++) {
        if (g_loadouts[i].classId != classId) {
            continue;
        }
        if (drawn % kCardsPerRow != 0) {
            ImGui::SameLine(0.0f, 12.0f * scale);
        } else {
            ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));
        }
        const size_t before = g_loadouts.size();
        DrawSavedCard(i, e, scale);
        if (g_loadouts.size() != before) {
            break;
        }
        drawn++;
    }
    if (drawn == 1) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));
        ImGui::TextDisabled("No saved loadouts for %s yet. Use \"Save as new\" to keep what you have equipped.",
                            kClassNames[classId]);
    }
    ImGui::EndChild();
}

}

bool CurrentEquipment(Equipment &out) {
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    if (g_snapshotValid) {
        out = g_snapshot;
    }
    return g_snapshotValid;
}

void DrawLoadoutsPanel(bool &open, float scale) {
    static int lastFrame = -2;
    const bool justOpened = ImGui::GetFrameCount() != lastFrame + 1;
    lastFrame = ImGui::GetFrameCount();

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float width = (kCardWidth * kCardsPerRow + 60.0f) * scale;
    const float height = 820.0f * scale;
    ImGui::SetNextWindowSize(ImVec2(width < screen.x * 0.95f ? width : screen.x * 0.95f,
                                    height < screen.y * 0.9f ? height : screen.y * 0.9f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Loadouts", &open)) {
        ImGui::End();
        return;
    }

    Equipment e;
    if (!CurrentEquipment(e)) {
        ImGui::TextUnformatted("Equipment not found yet.");
        ImGui::TextDisabled("Load your save and go to the lobby; it's picked up within a few seconds.");
        ImGui::End();
        return;
    }

    if (!g_status.empty() && ImGui::GetTime() < g_statusUntil) {
        if (g_statusIsError) {
            ImGui::TextColored(kWarning, "%s", g_status.c_str());
        } else {
            ImGui::TextColored(kLevel, "%s", g_status.c_str());
        }
    }

    if (ImGui::BeginTabBar("classes")) {
        for (int c = 0; c < kClassCount; c++) {
            char label[64];
            snprintf(label, sizeof(label), "%s%s###class%d", kClassNames[c],
                     c == e.activeClass ? " (active)" : "", c);
            const ImGuiTabItemFlags flags =
                justOpened && c == e.activeClass ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(label, nullptr, flags)) {
                DrawClassTab(c, e, scale);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
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
