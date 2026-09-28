#pragma once

#include <windows.h>

#include <string>
#include <vector>

// One upgradable number of a stat, as WEAPONTEXT stores it: base is the value at star 5.
struct StarValue {
    float base = 0.0f;
    int type = 0;
    int upgrade = 0;
    int maxLevel = 0;
    float a = 0.0f;
    float b = 0.0f;
    bool fractional = true;
};

// A stat line: format uses $0, $1... for its values. A stat without values ("Zoom", "----")
// is shown as its format.
struct StatSpec {
    std::string label;
    std::string format;
    std::vector<StarValue> values;
};

struct Weapon {
    int index = 0;
    std::vector<StatSpec> statSpecs;
    std::string name;
    std::string categoryName;
    std::string stats;
    int level = 0;
    int category = 0;
    bool owned = false;

    // Upgrade cap of each stat (from WEAPONTABLE) against the current level (from
    // the save). When every stat reaches its cap, the game puts a star on the name.
    std::vector<int> upgradeMax;
    std::vector<int> upgradeNow;
    bool starred = false;

    bool wish = false;

    // A mission's crates only roll weapons of its tier or lower: base=0,
    // MissionPack A=1, B=2. farmable says whether any mission can drop it.
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

// Community farming strategies, in Mods/Compendium/strats.tsv.
struct Strat {
    std::string mission;      // mission number, or "any"
    std::string difficulty;
    std::string className;
    std::string title;
    std::string source;
    std::string body;
};

void LoadStrats();
const std::vector<Strat> &GetStrats();

// Real owned state, decrypting MAIN.GST.
bool LeerObtenidasDelSave();
unsigned long long MomentoLecturaSave();

// Indices that went from not owned to owned in the last save reread. Empty on
// the first read, which compares against the TSV and not against a play session.
const std::vector<int> &RecienObtenidas();
bool SaveCambio();
const char *SavePathUsado();
int MarcadasEnSave();

const Catalog &GetCatalog();
void LoadCatalog();
Catalog &MutableCatalog();
Weapon *ArmaPorIndice(int index);

// Wishlist, in Mods/Compendium/wishlist.txt. Indexed by WEAPONTABLE index, not
// by name: some weapons share a name.
void LoadWishlist();
bool SaveWishlist();
int EnWishlist();

void Log(const char *msg);
void LogF(const char *fmt, ...);

std::string GamePath(const char *rel);

int TeclaToggle();

// Whether the focus filter is on. Turned off with "focus=0" in config.ini for
// machines where the window in front is never the game's.
bool FiltroDeFoco();

// Turns off direct polling with GetAsyncKeyState ("poll=0"), to check that the
// other two input paths work on a machine where the direct one does. Without
// this, the fallback path never gets exercised.
bool SondeoDirecto();

void InitOverlay();
