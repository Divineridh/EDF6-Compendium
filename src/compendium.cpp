#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <PluginAPI.h>

#include "compendium.h"
#include "loadouts.h"

static Catalog g_catalog;

// Ruta relativa al ejecutable del juego, no al directorio actual.
static std::string GamePath(const char *rel) {
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

// index | clase | catId | catNombre | nivel | nombre | obtenida | stats (tabs)
void LoadCatalog() {
    const std::string ruta = GamePath("Mods\\Compendium\\weapons.tsv");
    std::ifstream in(ruta);
    if (!in) {
        LogF("no encontre %s", ruta.c_str());
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
        std::string topes, tier;
        std::getline(ss, topes, '\t');
        std::getline(ss, tier, '\t');

        Weapon w;
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
    LogF("catalogo cargado: %d armas en %d clases", n, (int)g_catalog.classes.size());
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

// indice | nombre. El nombre va solo para poder leer el archivo a ojo.
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
    LogF("wishlist: %d armas marcadas", n);
}

bool SaveWishlist() {
    g_wishPath = GamePath("Mods\\Compendium\\wishlist.txt");
    std::ofstream out(g_wishPath);
    if (!out) {
        LogF("no pude escribir %s", g_wishPath.c_str());
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

// Valor de una clave de Mods\Compendium\config.ini, buscando los dos nombres
// (castellano e ingles). Cadena vacia si el archivo o la clave no estan.
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

// Tecla del overlay. Si algo mas se queda con F1 —el overlay de la placa de
// video, por ejemplo— se cambia sin recompilar poniendo en
// Mods\Compendium\config.ini una linea "tecla=0x71" con el codigo virtual.
static int LeerTecla(const char *clave, const char *alias, int porDefecto, const char *uso) {
    int tecla = porDefecto;
    const std::string valor = ValorDeConfig(clave, alias);
    if (!valor.empty()) {
        const int v = (int)strtol(valor.c_str(), nullptr, 0);
        if (v > 0 && v < 256) {
            tecla = v;
        }
    }
    LogF("tecla de %s: 0x%02X (%s)", uso, tecla,
         valor.empty() ? "por defecto, config.ini ausente o sin la clave" : "leida de config.ini");
    return tecla;
}

int TeclaToggle() {
    static const int tecla = LeerTecla("tecla", "key", VK_F1, "toggle");
    return tecla;
}

int TeclaLoadouts() {
    static const int tecla = LeerTecla("tecla_loadouts", "loadouts_key", VK_F2, "loadouts");
    return tecla;
}

// El filtro de foco evita que la tecla dispare mientras estas en otra
// aplicacion, pero depende de que GetForegroundWindow devuelva una ventana de
// este proceso, y hay maquinas donde eso no pasa nunca. Ademas de apagarse solo
// cuando lo detecta, se puede forzar con "foco=0".
bool FiltroDeFoco() {
    static int estado = -1;
    if (estado >= 0) {
        return estado != 0;
    }
    estado = 0;
    const std::string valor = ValorDeConfig("foco", "focus");
    if (valor == "1" || valor == "si" || valor == "on" || valor == "true" || valor == "yes") {
        estado = 1;
        Log("filtro de foco encendido por config.ini");
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
        Log("sondeo directo apagado por config.ini: la tecla tiene que entrar "
            "por el mensaje de ventana o por la lectura del juego");
    }
    return estado != 0;
}

static std::string g_savePath;

const char *SavePath() {
    return g_savePath.c_str();
}

// Escribe los nombres marcados. Es el mismo archivo que lee el generador del
// capitulo del manual, asi que marcar aca actualiza las dos salidas.
bool SaveOwned() {
    g_savePath = GamePath("Mods\\Compendium\\obtenidas.txt");
    std::ofstream out(g_savePath);
    if (!out) {
        LogF("no pude escribir %s", g_savePath.c_str());
        return false;
    }
    int n = 0;
    for (const ClassData &c : g_catalog.classes) {
        for (const Weapon &w : c.weapons) {
            if (w.owned) {
                out << w.name << "\n";
                n++;
            }
        }
    }
    LogF("guardadas %d armas en %s", n, g_savePath.c_str());
    return true;
}

static std::vector<Drop> g_drops;

const std::vector<Drop> &GetDrops() {
    return g_drops;
}

// dificultad | mision | nombre | nivelMin | nivelMax | probabilidad (separados por tab)
void LoadDrops() {
    const std::string ruta = GamePath("Mods\\Compendium\\missions.tsv");
    std::ifstream in(ruta);
    if (!in) {
        LogF("no encontre %s", ruta.c_str());
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
    LogF("tabla de dropeo: %d combinaciones mision/dificultad", (int)g_drops.size());
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
    LogF("armas que ninguna caja suelta: %d (premio de mision o DLC)", sin);
}

static std::vector<Strat> g_strats;

const std::vector<Strat> &GetStrats() {
    return g_strats;
}

// mision | dificultad | clase | titulo | fuente | cuerpo (separados por tab)
// El cuerpo trae los saltos de linea escapados como \n literal.
void LoadStrats() {
    const std::string ruta = GamePath("Mods\\Compendium\\strats.tsv");
    std::ifstream in(ruta);
    if (!in) {
        LogF("sin estrategias: no encontre %s", ruta.c_str());
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
    // Si el save se puede leer, manda el: tiene el estado real y ademas incluye
    // armas que la pantalla de equipamiento oculta por el limite de nivel.
    TeclaToggle();
    TeclaLoadouts();
    FiltroDeFoco();
    SondeoDirecto();
    MarcarFarmeables();
    LeerObtenidasDelSave();
    LoadWishlist();
    InitOverlay();
    InitLoadouts();
    return 0;
}

extern "C" BOOL __declspec(dllexport) EML6_Load(PluginInfo *pluginInfo) {
    pluginInfo->infoVersion = PluginInfo::MaxInfoVer;
    pluginInfo->name = "Weapon Compendium";
    pluginInfo->version = PLUG_VER(0, 1, 0, 0);
    LogF("EML6_Load llamado por el loader (build %s %s)", __DATE__, __TIME__);
    static bool arrancado = false;
    if (arrancado) {
        Log("ya estaba arrancado: ignoro esta segunda carga");
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
