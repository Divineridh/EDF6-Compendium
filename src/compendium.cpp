#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <PluginAPI.h>

#include "compendium.h"
#include "modules.h"
#include "weapon_stats.h"

static Catalog g_catalog;

// Path relative to the game's executable, not to the current directory.
std::string GamePath(const char *rel) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) {
        *(slash + 1) = '\0';
    }
    return std::string(path) + rel;
}

void Log(const char *msg) {
    FILE *fh = nullptr;
    if (fopen_s(&fh, GamePath("Compendium.log").c_str(), "a") == 0 && fh) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(fh, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
        fclose(fh);
    }
}

void LogF(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(buf);
}

const Catalog &GetCatalog() {
    return g_catalog;
}

static ClassData &ClasePorNombre(const std::string &nombre) {
    for (ClassData &c : g_catalog.classes) {
        if (c.name == nombre) {
            return c;
        }
    }
    g_catalog.classes.push_back(ClassData{nombre, {}});
    return g_catalog.classes.back();
}

// index | class | category id | category name | level | name | owned | stats (tab separated)
void LoadCatalog() {
    const std::string ruta = GamePath("Mods\\Compendium\\weapons.tsv");
    std::ifstream in(ruta);
    if (!in) {
        LogF("couldn't find %s", ruta.c_str());
        return;
    }

    std::string linea;
    int n = 0;
    while (std::getline(in, linea)) {
        if (linea.empty() || linea[0] == '#') {
            continue;
        }
        std::istringstream ss(linea);
        std::string idx, clase, catId, catNombre, nivel, nombre, obtenida, stats;
        if (!std::getline(ss, idx, '\t') || !std::getline(ss, clase, '\t') ||
            !std::getline(ss, catId, '\t') || !std::getline(ss, catNombre, '\t') ||
            !std::getline(ss, nivel, '\t') || !std::getline(ss, nombre, '\t')) {
            continue;
        }
        std::getline(ss, obtenida, '\t');
        std::getline(ss, stats, '\t');
        std::string topes, tier, specs;
        std::getline(ss, topes, '\t');
        std::getline(ss, tier, '\t');
        std::getline(ss, specs, '\t');

        Weapon w;
        w.statSpecs = ParseStatSpecs(specs);
        for (size_t i = 0, j = 0; i <= topes.size(); i++) {
            if (i == topes.size() || topes[i] == ',') {
                if (i > j) {
                    w.upgradeMax.push_back(atoi(topes.substr(j, i - j).c_str()));
                }
                j = i + 1;
            }
        }
        w.index = atoi(idx.c_str());
        w.name = nombre;
        w.categoryName = catNombre;
        w.stats = stats;
        w.level = atoi(nivel.c_str());
        w.category = atoi(catId.c_str());
        w.owned = (obtenida == "1");
        w.tier = atoi(tier.c_str());
        ClasePorNombre(clase).weapons.push_back(w);
        n++;
    }
    LogF("catalog loaded: %d weapons in %d classes", n, (int)g_catalog.classes.size());
}

Catalog &MutableCatalog() {
    return g_catalog;
}

Weapon *ArmaPorIndice(int index) {
    for (ClassData &c : g_catalog.classes) {
        for (Weapon &w : c.weapons) {
            if (w.index == index) {
                return &w;
            }
        }
    }
    return nullptr;
}

static std::string g_wishPath;

int EnWishlist() {
    int n = 0;
    for (const ClassData &c : g_catalog.classes) {
        for (const Weapon &w : c.weapons) {
            if (w.wish) {
                n++;
            }
        }
    }
    return n;
}

// index | name. The name is only there so the file can be read by eye.
void LoadWishlist() {
    g_wishPath = GamePath("Mods\\Compendium\\wishlist.txt");
    std::ifstream in(g_wishPath);
    if (!in) {
        return;
    }
    std::string linea;
    int n = 0;
    while (std::getline(in, linea)) {
        if (linea.empty() || linea[0] == '#') {
            continue;
        }
        Weapon *w = ArmaPorIndice(atoi(linea.c_str()));
        if (w) {
            w->wish = true;
            n++;
        }
    }
    LogF("wishlist: %d weapons marked", n);
}

bool SaveWishlist() {
    g_wishPath = GamePath("Mods\\Compendium\\wishlist.txt");
    std::ofstream out(g_wishPath);
    if (!out) {
        LogF("couldn't write %s", g_wishPath.c_str());
        return false;
    }
    for (const ClassData &c : g_catalog.classes) {
        for (const Weapon &w : c.weapons) {
            if (w.wish) {
                out << w.index << "\t" << w.name << "\n";
            }
        }
    }
    return true;
}

static void Recortar(std::string &s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    s.erase(0, i);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
}

// Value of a key in Mods\Compendium\config.ini, trying both names (Spanish and
// English). Empty string if the file or the key isn't there.
static std::string ValorDeConfig(const char *clave, const char *alias) {
    std::ifstream in(GamePath("Mods\\Compendium\\config.ini"));
    if (!in) {
        return std::string();
    }
    std::string linea;
    while (std::getline(in, linea)) {
        const size_t igual = linea.find('=');
        if (igual == std::string::npos) {
            continue;
        }
        std::string k = linea.substr(0, igual);
        std::string v = linea.substr(igual + 1);
        Recortar(k);
        Recortar(v);
        if (k == clave || k == alias) {
            return v;
        }
    }
    return std::string();
}

// Overlay key. If something else takes F1 (the graphics card overlay, for
// example), it can be changed without recompiling with a "key=0x71" line in
// Mods\Compendium\config.ini holding the virtual-key code.
static int LeerTecla(const char *clave, const char *alias, int porDefecto, const char *uso) {
    int tecla = porDefecto;
    const std::string valor = ValorDeConfig(clave, alias);
    if (!valor.empty()) {
        const int v = (int)strtol(valor.c_str(), nullptr, 0);
        if (v > 0 && v < 256) {
            tecla = v;
        }
    }
    LogF("%s key: 0x%02X (%s)", uso, tecla,
         valor.empty() ? "default, config.ini missing or without the setting" : "read from config.ini");
    return tecla;
}

int TeclaToggle() {
    static const int tecla = LeerTecla("tecla", "key", VK_F1, "toggle");
    return tecla;
}

// The focus filter keeps the key from firing while you are in another
// application, but it depends on GetForegroundWindow returning a window of this
// process, and on some machines that never happens. Besides turning itself off
// when it detects that, it can be forced off with "focus=0".
bool FiltroDeFoco() {
    static int estado = -1;
    if (estado >= 0) {
        return estado != 0;
    }
    estado = 0;
    const std::string valor = ValorDeConfig("foco", "focus");
    if (valor == "1" || valor == "si" || valor == "on" || valor == "true" || valor == "yes") {
        estado = 1;
        Log("focus filter turned on by config.ini");
    }
    return estado != 0;
}

bool SondeoDirecto() {
    static int estado = -1;
    if (estado >= 0) {
        return estado != 0;
    }
    estado = 1;
    const std::string valor = ValorDeConfig("sondeo", "poll");
    if (valor == "0" || valor == "no" || valor == "off" || valor == "false") {
        estado = 0;
        Log("direct polling turned off by config.ini: the key has to come in "
            "through the window message or the game's own read");
    }
    return estado != 0;
}

static std::vector<Drop> g_drops;

const std::vector<Drop> &GetDrops() {
    return g_drops;
}

// difficulty | mission | name | min level | max level | chance (tab separated)
void LoadDrops() {
    const std::string ruta = GamePath("Mods\\Compendium\\missions.tsv");
    std::ifstream in(ruta);
    if (!in) {
        LogF("couldn't find %s", ruta.c_str());
        return;
    }
    std::string linea;
    while (std::getline(in, linea)) {
        std::istringstream ss(linea);
        std::string dif, mis, nombre, lo, hi, prob;
        if (!std::getline(ss, dif, '\t') || !std::getline(ss, mis, '\t') ||
            !std::getline(ss, nombre, '\t') || !std::getline(ss, lo, '\t') ||
            !std::getline(ss, hi, '\t') || !std::getline(ss, prob, '\t')) {
            continue;
        }
        Drop d;
        d.difficulty = dif;
        d.mission = mis;
        d.missionName = nombre;
        d.lo = (float)atof(lo.c_str());
        d.hi = (float)atof(hi.c_str());
        d.chance = (float)atof(prob.c_str());
        std::string tier;
        if (std::getline(ss, tier, '\t')) {
            d.tier = atoi(tier.c_str());
        }
        g_drops.push_back(d);
    }
    LogF("drop table: %d mission/difficulty combinations", (int)g_drops.size());
}

void MarcarFarmeables() {
    int sin = 0;
    for (ClassData &c : g_catalog.classes) {
        for (Weapon &w : c.weapons) {
            w.farmable = false;
            for (const Drop &d : g_drops) {
                if (w.tier <= d.tier && d.lo <= w.level && w.level <= d.hi) {
                    w.farmable = true;
                    break;
                }
            }
            if (!w.farmable) {
                sin++;
            }
        }
    }
    LogF("weapons no crate drops: %d (mission rewards or DLC)", sin);
}

static std::vector<Strat> g_strats;

const std::vector<Strat> &GetStrats() {
    return g_strats;
}

// mission | difficulty | class | title | source | body (tab separated)
// Line breaks in the body are escaped as a literal \n.
void LoadStrats() {
    const std::string ruta = GamePath("Mods\\Compendium\\strats.tsv");
    std::ifstream in(ruta);
    if (!in) {
        LogF("no strategies: couldn't find %s", ruta.c_str());
        return;
    }
    std::string linea;
    while (std::getline(in, linea)) {
        if (linea.empty() || linea[0] == '#') {
            continue;
        }
        std::istringstream ss(linea);
        Strat s;
        if (!std::getline(ss, s.mission, '\t') || !std::getline(ss, s.difficulty, '\t') ||
            !std::getline(ss, s.className, '\t') || !std::getline(ss, s.title, '\t')) {
            continue;
        }
        std::getline(ss, s.source, '\t');
        std::string cuerpo;
        std::getline(ss, cuerpo, '\t');
        for (size_t i = 0; i < cuerpo.size(); i++) {
            if (cuerpo[i] == '\\' && i + 1 < cuerpo.size() && cuerpo[i + 1] == 'n') {
                s.body += '\n';
                i++;
            } else {
                s.body += cuerpo[i];
            }
        }
        g_strats.push_back(s);
    }
    LogF("estrategias cargadas: %d", (int)g_strats.size());
}

static DWORD WINAPI MainThread(LPVOID) {
    LoadCatalog();
    LoadDrops();
    LoadStrats();
    // If the save can be read, it wins: it has the real state and also includes
    // weapons the equipment screen hides because of the level cap.
    TeclaToggle();
    FiltroDeFoco();
    SondeoDirecto();
    MarcarFarmeables();
    LeerObtenidasDelSave();
    LoadWishlist();
    MarkCatalogReady();
    InitOverlay();
    return 0;
}

extern "C" BOOL __declspec(dllexport) EML6_Load(PluginInfo *pluginInfo) {
    pluginInfo->infoVersion = PluginInfo::MaxInfoVer;
    pluginInfo->name = "Weapon Compendium";
    pluginInfo->version = PLUG_VER(0, 5, 0, 0);
    LogF("EML6_Load called by the loader (build %s %s)", __DATE__, __TIME__);
    static bool arrancado = false;
    if (arrancado) {
        Log("already started: ignoring this second load");
        return TRUE;
    }
    arrancado = true;
    CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE modulo, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(modulo);
    }
    return TRUE;
}
