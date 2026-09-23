#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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

std::vector<int> g_classOfWeapon;
std::atomic<const uint32_t *> g_table{nullptr};

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

DWORD WINAPI WatchThread(LPVOID) {
    Equipment last;
    bool haveLast = false;
    bool reportedMissing = false;
    for (;;) {
        Equipment now;
        if (!ReadEquipment(now)) {
            if (!reportedMissing) {
                Log("loadouts: tabla no encontrada; reintento cada 5 s (normal antes de cargar la partida)");
                reportedMissing = true;
                haveLast = false;
            }
            Sleep(kRescanMs);
            continue;
        }
        reportedMissing = false;
        if (!haveLast || memcmp(&now, &last, sizeof(now)) != 0) {
            LogEquipment(now);
            last = now;
            haveLast = true;
        }
        Sleep(kPollMs);
    }
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
