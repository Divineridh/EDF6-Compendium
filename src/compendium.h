#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct Weapon {
    int index = 0;
    std::string name;
    std::string categoryName;
    std::string stats;
    int level = 0;
    int category = 0;
    bool owned = false;

    // Tope de mejora de cada stat (de WEAPONTABLE) contra el nivel actual (del
    // save). Cuando todos llegan al tope el juego le pone estrella al nombre.
    std::vector<int> upgradeMax;
    std::vector<int> upgradeNow;
    bool starred = false;

    bool wish = false;

    // Las cajas de una mision solo sortean armas de su tier o menor: base=0,
    // MissionPack A=1, B=2. farmable resume si alguna mision puede soltarla.
    int tier = 0;
    bool farmable = false;
};

struct ClassData {
    std::string name;
    std::vector<Weapon> weapons;
};

struct Catalog {
    std::vector<ClassData> classes;
};

struct Drop {
    int tier = 0;
    std::string difficulty;
    std::string mission;
    std::string missionName;
    float lo = 0.0f;
    float hi = 0.0f;
    float chance = 0.0f;
};

void LoadDrops();
const std::vector<Drop> &GetDrops();
void MarcarFarmeables();

// Estrategias de farmeo de la comunidad, en Mods/Compendium/strats.tsv.
struct Strat {
    std::string mission;      // numero de mision, o "any"
    std::string difficulty;
    std::string className;
    std::string title;
    std::string source;
    std::string body;
};

void LoadStrats();
const std::vector<Strat> &GetStrats();

// Estado real de obtenidas, descifrando MAIN.GST.
bool LeerObtenidasDelSave();

// Indices que pasaron de no obtenidas a obtenidas en la ultima relectura del
// save. Vacio en la primera, que compara contra el TSV y no contra una partida.
const std::vector<int> &RecienObtenidas();
bool SaveCambio();
const char *SavePathUsado();
int MarcadasEnSave();

const Catalog &GetCatalog();
void LoadCatalog();
Catalog &MutableCatalog();
Weapon *ArmaPorIndice(int index);

// Lista de deseados, en Mods/Compendium/wishlist.txt. Se indexa por indice de
// WEAPONTABLE y no por nombre: hay armas con nombre repetido.
void LoadWishlist();
bool SaveWishlist();
int EnWishlist();
bool SaveOwned();
const char *SavePath();

void Log(const char *msg);
void LogF(const char *fmt, ...);

int TeclaToggle();

// Si el filtro de foco esta habilitado. Se apaga con "foco=0" en config.ini para
// las maquinas donde la ventana de adelante nunca es la del juego.
bool FiltroDeFoco();

// Apaga el sondeo directo con GetAsyncKeyState ("sondeo=0"), para poder probar
// que los otros dos caminos de entrada andan en una maquina donde el directo
// funciona. Sin esto, el camino de respaldo nunca se ejercita.
bool SondeoDirecto();

void InitOverlay();
