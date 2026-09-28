// Reads the owned weapons straight from the game's save.
//
// MAIN.GST is encrypted with AES-256-CTR. The key and IV come from the file
// name:
//     key = MD5(utf16le("edf6MAIN.GST.sav")) + "Edf5.*_Steam_Ver"
//     iv  = MD5(utf16le("edf6MAIN.GST.stm"))
// The plaintext starts with the magic "MDB".
//
// Algorithm published in EDFDecrypt.cpp of FevGrave's EDFSaveEditor, with
// credit to Quarri6343 for discovering it.
//
// Inside, at 0x7CFC, there's a table of 2048 entries of 12 bytes indexed like
// WEAPONTABLE. An all-zero entry is a weapon you don't have; anything else means
// owned.
//
// Careful: the save marks more weapons than the equipment screen shows, because
// that screen also filters by the level cap you haven't unlocked yet. The save
// is the right source.

#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>

#include "compendium.h"

#pragma comment(lib, "bcrypt.lib")

static const size_t TABLA = 0x7CFC;
static const size_t PASO = 12;

// ------------------------------------------------------------------ cripto

static bool Md5(const void *datos, size_t largo, unsigned char salida[16]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, (PUCHAR)datos, (ULONG)largo, 0) == 0 &&
                BCryptFinishHash(hash, salida, 16, 0) == 0) {
                ok = true;
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

// CTR doesn't use block decryption: it encrypts the counter and XORs. So AES in
// ECB mode, which is what CNG exposes, is enough.
static bool AesCtr(std::vector<unsigned char> &datos, const unsigned char key[32],
                   const unsigned char iv[16]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    bool ok = false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == 0) {
        BCRYPT_KEY_HANDLE llave = nullptr;
        if (BCryptGenerateSymmetricKey(alg, &llave, nullptr, 0, (PUCHAR)key, 32, 0) == 0) {
            unsigned char contador[16];
            memcpy(contador, iv, 16);
            unsigned char keystream[16];
            ok = true;
            for (size_t off = 0; off < datos.size(); off += 16) {
                ULONG escritos = 0;
                if (BCryptEncrypt(llave, contador, 16, nullptr, nullptr, 0, keystream,
                                  sizeof(keystream), &escritos, 0) != 0) {
                    ok = false;
                    break;
                }
                const size_t n = min((size_t)16, datos.size() - off);
                for (size_t i = 0; i < n; i++) {
                    datos[off + i] ^= keystream[i];
                }
                for (int i = 15; i >= 0; i--) {   // incremento big-endian
                    if (++contador[i] != 0) break;
                }
            }
            BCryptDestroyKey(llave);
        }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ------------------------------------------------------------- location

// The folder is "EarthDefenceForce6", with a C: searching for "Defense" misses it.
// Inside there's a folder per steamid and several slots, so the most recently
// modified MAIN.GST is picked.
static bool BuscarSave(std::string &rutaFinal, FILETIME &escrituraFinal) {
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) {
        return false;
    }
    const std::string raiz = std::string(base) + "\\EarthDefenceForce6\\SAVE_DATA\\";

    bool encontrado = false;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((raiz + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') {
            continue;
        }
        const std::string cuenta = raiz + fd.cFileName + "\\";
        WIN32_FIND_DATAA fd2;
        HANDLE h2 = FindFirstFileA((cuenta + "saveslot*").c_str(), &fd2);
        if (h2 == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                continue;
            }
            const std::string candidato = cuenta + fd2.cFileName + "\\MAIN.GST";
            WIN32_FILE_ATTRIBUTE_DATA info;
            if (!GetFileAttributesExA(candidato.c_str(), GetFileExInfoStandard, &info)) {
                continue;
            }
            if (!encontrado || CompareFileTime(&info.ftLastWriteTime, &escrituraFinal) > 0) {
                rutaFinal = candidato;
                escrituraFinal = info.ftLastWriteTime;
                encontrado = true;
            }
        } while (FindNextFileA(h2, &fd2));
        FindClose(h2);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return encontrado;
}

// ------------------------------------------------------------------ lectura

static std::string g_rutaSave;
static FILETIME g_escrituraSave = {0, 0};
static int g_marcadasSave = 0;
static std::vector<int> g_recien;
static bool g_primeraLectura = true;
static unsigned long long g_leidoEn = 0;

unsigned long long MomentoLecturaSave() {
    return g_leidoEn;
}

const std::vector<int> &RecienObtenidas() {
    return g_recien;
}

const char *SavePathUsado() {
    return g_rutaSave.c_str();
}

int MarcadasEnSave() {
    return g_marcadasSave;
}

bool SaveCambio() {
    if (g_rutaSave.empty()) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(g_rutaSave.c_str(), GetFileExInfoStandard, &info)) {
        return false;
    }
    return CompareFileTime(&info.ftLastWriteTime, &g_escrituraSave) != 0;
}

bool LeerObtenidasDelSave() {
    std::string ruta;
    FILETIME escritura = {0, 0};
    if (!BuscarSave(ruta, escritura)) {
        Log("couldn't find any MAIN.GST");
        return false;
    }

    HANDLE h = CreateFileA(ruta.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogF("couldn't open %s", ruta.c_str());
        return false;
    }
    const DWORD largo = GetFileSize(h, nullptr);
    if (largo == INVALID_FILE_SIZE || largo < TABLA || largo > (64u << 20)) {
        LogF("save size out of range: %lu", largo);
        CloseHandle(h);
        return false;
    }
    std::vector<unsigned char> datos(largo);
    DWORD leidos = 0;
    const BOOL bien = ReadFile(h, datos.data(), largo, &leidos, nullptr);
    CloseHandle(h);
    if (!bien || leidos != largo) {
        Log("couldn't read the whole save");
        return false;
    }

    const wchar_t *nombreClave = L"edf6MAIN.GST.sav";
    const wchar_t *nombreIv = L"edf6MAIN.GST.stm";
    unsigned char key[32];
    unsigned char iv[16];
    if (!Md5(nombreClave, wcslen(nombreClave) * sizeof(wchar_t), key) ||
        !Md5(nombreIv, wcslen(nombreIv) * sizeof(wchar_t), iv)) {
        Log("MD5 failed");
        return false;
    }
    memcpy(key + 16, "Edf5.*_Steam_Ver", 16);

    if (!AesCtr(datos, key, iv)) {
        Log("AES decryption failed");
        return false;
    }
    if (datos.size() < 3 || memcmp(datos.data(), "MDB", 3) != 0) {
        Log("the save didn't decrypt: the MDB magic is missing");
        return false;
    }
    if (datos.size() <= TABLA) {
        Log("the save is smaller than the weapon table");
        return false;
    }

    // The table is 2048 fixed entries. Deriving the count from the file size
    // works today because the table ends right at the end of the file, but a
    // patch that appends data would read garbage as owned weapons.
    size_t entradas = (datos.size() - TABLA) / PASO;
    if (entradas > 2048) {
        entradas = 2048;
    }
    g_recien.clear();
    int marcadas = 0;
    for (ClassData &c : MutableCatalog().classes) {
        for (Weapon &w : c.weapons) {
            if ((size_t)w.index >= entradas) {
                continue;
            }
            const unsigned char *entrada = datos.data() + TABLA + (size_t)w.index * PASO;
            unsigned int valor = 0;
            memcpy(&valor, entrada, 4);
            const bool antes = w.owned;
            w.owned = (valor != 0);
            if (w.owned) {
                marcadas++;
            }
            if (w.owned && !antes && !g_primeraLectura) {
                g_recien.push_back(w.index);
            }

            // The next 8 bytes are the upgrade level of each stat.
            w.upgradeNow.assign(entrada + 4, entrada + PASO);
            w.starred = w.owned && !w.upgradeMax.empty();
            for (size_t i = 0; i < w.upgradeMax.size(); i++) {
                const int actual = i < w.upgradeNow.size() ? w.upgradeNow[i] : 0;
                if (actual < w.upgradeMax[i]) {
                    w.starred = false;
                }
            }
        }
    }

    g_rutaSave = ruta;
    g_escrituraSave = escritura;
    g_marcadasSave = marcadas;
    if (!g_recien.empty()) {
        LogF("new since the last read: %d", (int)g_recien.size());
    }
    g_primeraLectura = false;
    g_leidoEn = GetTickCount64();
    LogF("save read: %d owned weapons from %s", marcadas, ruta.c_str());
    return true;
}
