// Lee las armas obtenidas directamente del save del juego.
//
// MAIN.GST esta cifrado con AES-256-CTR. La clave y el IV salen del nombre del
// archivo:
//     clave = MD5(utf16le("edf6MAIN.GST.sav")) + "Edf5.*_Steam_Ver"
//     iv    = MD5(utf16le("edf6MAIN.GST.stm"))
// El texto plano arranca con el magic "MDB".
//
// Algoritmo publicado en EDFDecrypt.cpp del EDFSaveEditor de FevGrave, con
// credito a Quarri6343 por descubrirlo.
//
// Adentro, en 0x7CFC, hay una tabla de 2048 entradas de 12 bytes indexada igual
// que WEAPONTABLE. Una entrada en cero es un arma que no tenes; cualquier cosa
// distinta de cero significa obtenida.
//
// Ojo: el save marca mas armas de las que muestra la pantalla de equipamiento,
// porque esa pantalla ademas filtra por el limite de nivel todavia no
// desbloqueado. El save es la fuente correcta.

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

// CTR no usa el descifrado de bloque: cifra el contador y hace XOR. Por eso
// alcanza con AES en modo ECB, que es lo que expone CNG.
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

// ------------------------------------------------------------- ubicacion

// La carpeta es "EarthDefenceForce6", con C: buscar "Defense" no la encuentra.
// Adentro hay una carpeta por steamid y varios slots, asi que se elige el
// MAIN.GST modificado mas recientemente.
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
        Log("no encontre ningun MAIN.GST");
        return false;
    }

    HANDLE h = CreateFileA(ruta.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogF("no pude abrir %s", ruta.c_str());
        return false;
    }
    const DWORD largo = GetFileSize(h, nullptr);
    if (largo == INVALID_FILE_SIZE || largo < TABLA || largo > (64u << 20)) {
        LogF("tamano de save fuera de rango: %lu", largo);
        CloseHandle(h);
        return false;
    }
    std::vector<unsigned char> datos(largo);
    DWORD leidos = 0;
    const BOOL bien = ReadFile(h, datos.data(), largo, &leidos, nullptr);
    CloseHandle(h);
    if (!bien || leidos != largo) {
        Log("no pude leer el save entero");
        return false;
    }

    const wchar_t *nombreClave = L"edf6MAIN.GST.sav";
    const wchar_t *nombreIv = L"edf6MAIN.GST.stm";
    unsigned char key[32];
    unsigned char iv[16];
    if (!Md5(nombreClave, wcslen(nombreClave) * sizeof(wchar_t), key) ||
        !Md5(nombreIv, wcslen(nombreIv) * sizeof(wchar_t), iv)) {
        Log("fallo el MD5");
        return false;
    }
    memcpy(key + 16, "Edf5.*_Steam_Ver", 16);

    if (!AesCtr(datos, key, iv)) {
        Log("fallo el descifrado AES");
        return false;
    }
    if (datos.size() < 3 || memcmp(datos.data(), "MDB", 3) != 0) {
        Log("el save no descifro bien: falta el magic MDB");
        return false;
    }
    if (datos.size() <= TABLA) {
        Log("el save es mas chico que la tabla de armas");
        return false;
    }

    // La tabla son 2048 entradas fijas. Derivar el tope del tamano del archivo
    // funciona hoy porque la tabla termina justo al final, pero un parche que
    // agregue datos atras haria leer basura como armas obtenidas.
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

            // Los 8 bytes que siguen son el nivel de mejora de cada stat.
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
        LogF("nuevas desde la ultima lectura: %d", (int)g_recien.size());
    }
    g_primeraLectura = false;
    g_leidoEn = GetTickCount64();
    LogF("save leido: %d armas obtenidas de %s", marcadas, ruta.c_str());
    return true;
}
