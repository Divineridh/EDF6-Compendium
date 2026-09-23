#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
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

const char *const kClassNames[kClassCount] = {"Ranger", "Wing Diver", "Air Raider", "Fencer"};

const ImVec4 kCardHeader(0.06f, 0.43f, 0.34f, 0.95f);
const ImVec4 kCardRow(0.03f, 0.31f, 0.25f, 0.90f);
const ImVec4 kCardGap(0.02f, 0.20f, 0.17f, 0.90f);
const ImVec4 kCardBorder(0.36f, 0.79f, 0.65f, 1.0f);
const ImVec4 kLevel(0.62f, 0.88f, 0.80f, 1.0f);
const ImVec4 kLevelMaxed(0.98f, 0.78f, 0.46f, 1.0f);

std::vector<int> g_classOfWeapon;
std::atomic<const uint32_t *> g_table{nullptr};

std::mutex g_snapshotMutex;
Equipment g_snapshot;
bool g_snapshotValid = false;

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

bool ScanRegion(const uint32_t *begin, size_t count, const uint32_t **found, int &foundCount) {
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

const uint32_t *FindTable() {
    const DWORD start = GetTickCount();
    const uint32_t *found[kMaxCandidates] = {};
    int foundCount = 0;
    size_t scannedBytes = 0;
    int faultedRegions = 0;

    MEMORY_BASIC_INFORMATION mbi;
    const char *addr = nullptr;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const char *base = (const char *)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            if (!ScanRegion((const uint32_t *)base, mbi.RegionSize / 4, found, foundCount)) {
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
    const uint32_t *table = g_table.load();
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

void RightAlignedText(const ImVec4 &color, const char *text) {
    const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(x);
    ImGui::TextColored(color, "%s", text);
}

void DrawCard(const char *id, const char *title, const int slots[kSlotsPerClass], float scale) {
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, kCardBorder);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kCardGap);
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable(id, 2, flags, ImVec2(430.0f * scale, 0.0f))) {
        ImGui::TableSetupColumn("weapon", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("level", ImGuiTableColumnFlags_WidthFixed, 56.0f * scale);

        ImGui::TableNextRow();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(kCardHeader));
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(title);

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

    if (!ImGui::Begin("Loadouts", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
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

    if (ImGui::BeginTabBar("classes")) {
        for (int c = 0; c < kClassCount; c++) {
            char label[64];
            snprintf(label, sizeof(label), "%s%s###class%d", kClassNames[c],
                     c == e.activeClass ? " (active)" : "", c);
            const ImGuiTabItemFlags flags =
                justOpened && c == e.activeClass ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(label, nullptr, flags)) {
                DrawCard("equipped", "Equipped now", e.slots[c], scale);
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
        int classId = -1;
        for (int i = 0; i < kClassCount; i++) {
            if (c.name == kClassNames[i]) {
                classId = i;
            }
        }
        for (const Weapon &w : c.weapons) {
            g_classOfWeapon[w.index] = classId;
        }
    }
    if (g_classOfWeapon.empty()) {
        Log("loadouts: sin catalogo, no busco la tabla");
        return;
    }
    LogF("loadouts: catalogo de %d armas; arranca la busqueda de la tabla", (int)g_classOfWeapon.size());
    CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
}
