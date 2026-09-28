#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "compendium.h"
#include "compendium_ui.h"
#include "modules.h"
#include "ui_kit.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef HRESULT(__stdcall *PresentFn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(__stdcall *ResizeBuffersFn)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn oPresent = nullptr;
static ResizeBuffersFn oResizeBuffers = nullptr;
static WNDPROC oWndProc = nullptr;

static ID3D11Device *g_device = nullptr;
static ID3D11DeviceContext *g_context = nullptr;
static ID3D11RenderTargetView *g_rtv = nullptr;
static HWND g_window = nullptr;
static bool g_imguiReady = false;
static std::atomic<bool> g_visible{false};
static std::atomic<int> g_panelModulo{-1};
static std::atomic<unsigned long long> g_frames{0};
static std::atomic<bool> g_toggleSolicitado{false};
static std::atomic<bool> g_toggleModuloSolicitado[kMaxModules];

static bool AlgunPanelVisible() {
    return g_visible || g_panelModulo >= 0;
}


// ---------------------------------------------------------------- interfaz


static std::vector<std::string> g_nuevas;
static std::vector<std::string> g_nuevasWish;
static unsigned long long g_toastHasta = 0;

static float g_escala = 1.0f;

const std::vector<std::string> &NuevasDesdeUltimaMision() {
    return g_nuevas;
}

void DescartarNuevas() {
    g_nuevas.clear();
    g_nuevasWish.clear();
}

// El save se revisa cada dos segundos y no por frame: alcanza de sobra para
// avisar al volver de una mision, y hay que hacerlo con el overlay cerrado para
// que el aviso llegue sin tener que abrirlo.
static void RevisarSave() {
    static unsigned long long proxima = 0;
    const unsigned long long ahora = GetTickCount64();
    if (ahora < proxima) {
        return;
    }
    proxima = ahora + 2000;
    if (!SaveCambio() || !LeerObtenidasDelSave()) {
        return;
    }
    g_nuevas.clear();
    g_nuevasWish.clear();
    for (int idx : RecienObtenidas()) {
        const Weapon *w = ArmaPorIndice(idx);
        if (!w) {
            continue;
        }
        g_nuevas.push_back(w->name);
        if (w->wish) {
            g_nuevasWish.push_back(w->name);
        }
    }
    if (!g_nuevasWish.empty()) {
        g_toastHasta = ahora + 9000;
    }
}

static bool ToastActivo() {
    return GetTickCount64() < g_toastHasta && !g_nuevasWish.empty();
}

static void DrawToast() {
    ui::SetScale(g_escala * 0.8f);
    const char *title = "Wishlisted weapon dropped";
    float width = ui::Measure(ui::g_fontSemi, 15.0f, title).x;
    for (const std::string &nombre : g_nuevasWish) {
        const float w = ui::Measure(nullptr, 14.0f, nombre.c_str()).x;
        width = w > width ? w : width;
    }
    const ImVec2 size(width + ui::D(60.0f), ui::D(46.0f) + ui::D(24.0f) * (float)g_nuevasWish.size());
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - ui::D(28.0f), vp->WorkPos.y + ui::D(28.0f)),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Rgb(ui::kBg, 0.95f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoInputs;
    const bool visible = ImGui::Begin("##toastWish", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (visible) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetWindowPos();
        dl->AddRect(o, ImVec2(o.x + size.x, o.y + size.y), ui::Rgb(ui::kLine), 0.0f, ui::D(1.0f));
        dl->AddRectFilled(o, ImVec2(o.x + ui::D(3.0f), o.y + size.y), ui::Rgb(ui::kPink));
        ui::Heart(dl, ImVec2(o.x + ui::D(24.0f), o.y + ui::D(23.0f)), ui::D(16.0f), ui::kPink);
        ui::PaintText(dl, ui::g_fontSemi, 15.0f, ImVec2(o.x + ui::D(40.0f), o.y + ui::D(13.0f)), ui::kText, title);
        float y = o.y + ui::D(42.0f);
        for (const std::string &nombre : g_nuevasWish) {
            ui::PaintText(dl, nullptr, 14.0f, ImVec2(o.x + ui::D(40.0f), y), ui::kSoft, nombre.c_str());
            y += ui::D(24.0f);
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- hooks

// ------------------------------------------------------- bloqueo de input
//
// EDF6 no lee el mouse por mensajes de ventana: importa GetKeyState,
// GetKeyboardState y GetCursorPos, o sea que consulta el estado directamente y
// se saltea el WndProc. Para que el overlay no dispare acciones del juego hay
// que mentirle a esas tres.
//
// El problema es que el backend Win32 de imgui usa las mismas funciones. Por eso
// el bloqueo se levanta durante nuestro NewFrame: adentro de esa ventana las
// llamadas son nuestras y pasan de largo; afuera son del juego y se neutralizan.

typedef SHORT(WINAPI *GetKeyStateFn)(int);
typedef BOOL(WINAPI *GetKeyboardStateFn)(PBYTE);
typedef BOOL(WINAPI *GetCursorPosFn)(LPPOINT);

static GetKeyStateFn oGetKeyState = nullptr;
static GetKeyboardStateFn oGetKeyboardState = nullptr;
static GetCursorPosFn oGetCursorPos = nullptr;

static bool g_enImGui = false;
static POINT g_cursorCongelado = {0, 0};

// Tercer camino de entrada, y el mas confiable: estas dos funciones ya estaban
// enganchadas para poder mutearle el teclado al juego, y GetKeyboardState trae
// el estado de las 256 teclas. Leyendo de ahi usamos exactamente el mismo dato
// que usa el juego: si el juego responde al teclado, esto responde tambien.
// No hace falta ninguna API nueva ni un hook global.
static std::atomic<bool> g_teclaEspiada{false};
static std::atomic<bool> g_teclaModuloEspiada[kMaxModules];
static std::atomic<bool> g_juegoLeeTeclado{false};

static bool Bloqueando() {
    return AlgunPanelVisible() && !g_enImGui;
}

static SHORT WINAPI hkGetKeyState(int vk) {
    const SHORT r = oGetKeyState(vk);
    g_juegoLeeTeclado = true;
    if (vk == TeclaToggle()) {
        g_teclaEspiada = (r & 0x8000) != 0;
    }
    for (int i = 0; i < ModuleCount(); i++) {
        if (vk == ModuleAt(i)->toggleKey) {
            g_teclaModuloEspiada[i] = (r & 0x8000) != 0;
        }
    }
    if (Bloqueando()) {
        return 0;
    }
    return r;
}

static BOOL WINAPI hkGetKeyboardState(PBYTE estado) {
    const BOOL r = oGetKeyboardState(estado);
    if (r && estado) {
        g_juegoLeeTeclado = true;
        const int vk = TeclaToggle();
        if (vk > 0 && vk < 256) {
            g_teclaEspiada = (estado[vk] & 0x80) != 0;
        }
        for (int i = 0; i < ModuleCount(); i++) {
            const int vkModulo = ModuleAt(i)->toggleKey;
            if (vkModulo > 0 && vkModulo < 256) {
                g_teclaModuloEspiada[i] = (estado[vkModulo] & 0x80) != 0;
            }
        }
        if (Bloqueando()) {
            memset(estado, 0, 256);
        }
    }
    return r;
}

static BOOL WINAPI hkGetCursorPos(LPPOINT p) {
    if (Bloqueando() && p) {
        *p = g_cursorCongelado;
        return TRUE;
    }
    return oGetCursorPos(p);
}

static void EngancharInput() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) {
        return;
    }
    struct {
        const char *nombre;
        void *reemplazo;
        void **original;
    } objetivos[] = {
        {"GetKeyState", &hkGetKeyState, (void **)&oGetKeyState},
        {"GetKeyboardState", &hkGetKeyboardState, (void **)&oGetKeyboardState},
        {"GetCursorPos", &hkGetCursorPos, (void **)&oGetCursorPos},
    };
    for (auto &o : objetivos) {
        void *dir = (void *)GetProcAddress(user32, o.nombre);
        if (!dir) {
            LogF("no encontre %s", o.nombre);
            continue;
        }
        if (MH_CreateHook(dir, o.reemplazo, o.original) != MH_OK ||
            MH_EnableHook(dir) != MH_OK) {
            LogF("no pude enganchar %s", o.nombre);
        }
    }
    Log("hooks de input instalados");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Hay dos caminos de entrada: este mensaje y el sondeo de GetAsyncKeyState
    // en RevisarToggle(). En algunas maquinas el sondeo nunca ve la tecla, asi
    // que aca se deja el pedido anotado y alla se resuelve; un cooldown evita
    // el doble disparo cuando llegan los dos.
    if (msg == WM_KEYDOWN && wp == (WPARAM)TeclaToggle()) {
        g_toggleSolicitado = true;
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        for (int i = 0; i < ModuleCount(); i++) {
            if (wp == (WPARAM)ModuleAt(i)->toggleKey) {
                g_toggleModuloSolicitado[i] = true;
                return 0;
            }
        }
    }
    if (AlgunPanelVisible() && g_imguiReady) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        const ImGuiIO &io = ImGui::GetIO();
        const bool teclado = io.WantCaptureKeyboard &&
                             (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR);
        const bool mouse = io.WantCaptureMouse &&
                           (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
        if (teclado || mouse) {
            return 1;
        }
    }
    if (!oWndProc) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return CallWindowProc(oWndProc, hwnd, msg, wp, lp);
}


// El render target se pierde en cada ResizeBuffers y puede fallar en el primer
// frame. Antes se creaba una sola vez en la init: si eso fallaba, el overlay
// quedaba mudo para siempre sin decir nada. Ahora se reintenta por frame.
static void AsegurarRenderTarget(IDXGISwapChain *swap) {
    if (g_rtv || !g_device) {
        return;
    }
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
    static bool avisado = false;
    if (!g_rtv && !avisado) {
        Log("todavia sin render target; se reintenta cada frame");
        avisado = true;
    }
}

static bool g_imguiCreado = false;

static void InitImGui(IDXGISwapChain *swap) {
    if (g_imguiCreado) {
        return;
    }
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void **)&g_device))) {
        static bool avisado = false;
        if (!avisado) {
            Log("no pude sacar el device del swapchain");
            avisado = true;
        }
        return;
    }
    g_imguiCreado = true;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc = {};
    swap->GetDesc(&desc);
    g_window = desc.OutputWindow;
    LogF("ventana del swapchain = %p", (void *)g_window);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    // La fuente por defecto de imgui es ProggyClean: 13px y solo ASCII, asi que
    // a 1080p queda ilegible y no puede dibujar la estrella. Cargamos Segoe UI y
    // le fusionamos Segoe UI Symbol solo para el glifo U+2605, que es la marca
    // que usa el juego para las armas maximizadas.
    ImFont *base = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    if (base) {
        static const ImWchar rangoEstrella[] = {0x2605, 0x2606, 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 16.0f, &cfg,
                                          rangoEstrella)) {
            Log("no pude fusionar el simbolo de estrella");
        }
    } else {
        // Sin la fuente del sistema seguimos con la de imgui: se ve peor pero anda.
        Log("no pude cargar segoeui.ttf, sigo con la fuente por defecto");
    }
    ui::LoadFonts();

    // io.FontGlobalScale se movio a style.FontScaleMain en imgui 1.92.
    const float escala = desc.BufferDesc.Height >= 1440 ? 2.0f : 1.6f;
    g_escala = escala;
    ImGui::GetStyle().FontScaleMain = escala;
    ImGui::GetStyle().ScaleAllSizes(escala);

    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);

    oWndProc = (WNDPROC)SetWindowLongPtr(g_window, GWLP_WNDPROC, (LONG_PTR)WndProc);
    g_imguiReady = true;
    Log("imgui inicializado");
}

// El toggle por WM_KEYDOWN depende de que el juego despache los mensajes a la
// ventana, y EDF6 los procesa en su propio loop de PeekMessage. Por eso ademas
// se sondea la tecla en cada frame: GetAsyncKeyState no pasa por la cola y no
// esta enganchada, asi que funciona igual.
// Se compara por PROCESO y no por handle de ventana: el juego puede tener en
// foco una ventana distinta a la del swapchain segun el modo de pantalla o las
// capas que tenga inyectadas, y comparando handles el toggle quedaba muerto para
// siempre. El objetivo del filtro es solo evitar que la tecla dispare mientras
// estas en otra aplicacion.

// Nombre del ejecutable que esta adelante. Solo para el log: cuando el filtro
// falla, esto es lo unico que distingue "no tenias el juego adelante" de "esta
// maquina no lo reporta".
static std::string ProcesoDeFoco(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return "?";
    }
    char ruta[MAX_PATH] = {0};
    DWORD largo = MAX_PATH;
    if (!QueryFullProcessImageNameA(h, 0, ruta, &largo)) {
        ruta[0] = '\0';
    }
    CloseHandle(h);
    const char *barra = strrchr(ruta, '\\');
    return barra ? barra + 1 : ruta;
}

// Cuanto se espera antes de dar el filtro por inservible.
static const ULONGLONG ESPERA_FOCO_MS = 8000;

// En la maquina de un tester GetForegroundWindow nunca devolvio una ventana del
// juego, con lo cual el filtro daba false en todos los frames y el overlay no
// abria nunca. Como el filtro es una comodidad y no una necesidad, si en los
// primeros segundos de partida no vemos ni una vez a este proceso adelante,
// asumimos que el dato no sirve en esta maquina y lo dejamos de mirar.
static bool JuegoEnFoco() {
    static bool filtro = true;
    static bool visto = false;
    static ULONGLONG desde = 0;

    if (!filtro || !FiltroDeFoco()) {
        return true;
    }

    HWND foco = GetForegroundWindow();
    DWORD pid = 0;
    if (foco) {
        GetWindowThreadProcessId(foco, &pid);
    }
    if (pid == GetCurrentProcessId()) {
        visto = true;
        return true;
    }

    if (!desde) {
        desde = GetTickCount64();
    }
    if (!visto && GetTickCount64() - desde > ESPERA_FOCO_MS) {
        LogF("el filtro de foco no sirve en esta maquina: adelante esta %s (pid %lu) y nunca "
             "vi este proceso (pid %lu) adelante. Lo apago, la tecla anda igual",
             ProcesoDeFoco(pid).c_str(), pid, GetCurrentProcessId());
        filtro = false;
        return true;
    }
    return false;
}

static bool g_algunaTecla = false;

// Barre todos los codigos de tecla mientras no se haya visto ninguna. Contesta
// la pregunta que ningun otro log contesta: si GetAsyncKeyState sirve para algo
// en este proceso. Si nunca imprime, el teclado no llega por esa via y da lo
// mismo que tecla este configurada. Solo se registra la primera tecla vista.
static void EscanearTeclado() {
    static bool visto = false;
    static unsigned long long proximo = 0;
    if (visto) {
        return;
    }
    const unsigned long long t = GetTickCount64();
    if (t < proximo) {
        return;
    }
    proximo = t + 100;
    if (g_teclaEspiada) {
        visto = true;
        Log("0b. la tecla llega por la lectura que hace el propio juego");
        return;
    }
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            visto = true;
            LogF("0b. el teclado SI responde: primera tecla vista 0x%02X (la configurada es 0x%02X)",
                 vk, TeclaToggle());
            return;
        }
    }
}

// Latido mientras no se detecto ninguna tecla: prueba que el bucle corre y que
// el foco no lo esta frenando. Sin esto, "no pasa nada" y "no llega la tecla"
// se ven igual en el log.
static void Latido(bool enFoco) {
    static unsigned long long proximo = 0;
    const unsigned long long t = GetTickCount64();
    if (g_algunaTecla || t < proximo) {
        return;
    }
    static unsigned long long framesPrevios = 0;
    const unsigned long long frames = g_frames;
    if (proximo) {
        LogF("0. sondeando: %llu frames en los ultimos 10s%s. Tecla 0x%02X sin detectar, "
             "el juego %s el teclado",
             frames - framesPrevios,
             (frames == framesPrevios && g_imguiReady) ? "  <-- NO NOS LLAMAN A DIBUJAR" : "",
             TeclaToggle(), g_juegoLeeTeclado ? "SI lee" : "NO lee");
    }
    framesPrevios = frames;
    proximo = t + 10000;
}

static const char *PulsacionNueva(int vk, bool espiada, bool porMensaje, bool &anterior) {
    const bool porApi = SondeoDirecto() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool ahora = porApi || espiada;
    const bool porSondeo = ahora && !anterior;
    anterior = ahora;
    if (!porSondeo && !porMensaje) {
        return nullptr;
    }
    return !porSondeo ? "mensaje de ventana" : (porApi ? "sondeo directo" : "lectura del propio juego");
}

static unsigned long long MsDesde(unsigned long long &ultimo) {
    const unsigned long long t = GetTickCount64();
    const unsigned long long delta = t - ultimo;
    if (delta >= 250) {
        ultimo = t;
    }
    return delta;
}

// Only one panel is open at a time: opening one closes the other. The cursor is frozen for the
// game when the first panel opens, not when switching between panels.
static void CongelarCursorSiHaceFalta(bool yaBloqueaba) {
    if (!yaBloqueaba && oGetCursorPos) {
        oGetCursorPos(&g_cursorCongelado);
    }
}

static void AlternarCompendium() {
    const bool yaBloqueaba = AlgunPanelVisible();
    g_visible = !g_visible;
    if (g_visible) {
        g_panelModulo = -1;
        CongelarCursorSiHaceFalta(yaBloqueaba);
    }
}

static void AlternarPanelModulo(int indice) {
    const bool yaBloqueaba = AlgunPanelVisible();
    if (g_panelModulo == indice) {
        g_panelModulo = -1;
        return;
    }
    g_panelModulo = indice;
    g_visible = false;
    CongelarCursorSiHaceFalta(yaBloqueaba);
}

static void RevisarToggle() {
    static bool anterior = false;
    static bool avisado = false;
    static bool avisadoFoco = false;
    static unsigned long long ultimoToggle = 0;
    static bool anteriorModulo[kMaxModules] = {};
    static unsigned long long ultimoToggleModulo[kMaxModules] = {};

    // Un WM_KEYDOWN solo llega si la ventana tiene el foco de teclado de verdad,
    // asi que ese camino no pasa por el filtro. En las maquinas donde
    // GetForegroundWindow miente, es el unico que queda.
    const bool porMensaje = g_toggleSolicitado.exchange(false);
    const int modulos = ModuleCount();
    bool porMensajeModulo[kMaxModules] = {};
    bool algunModuloPorMensaje = false;
    for (int i = 0; i < modulos; i++) {
        porMensajeModulo[i] = g_toggleModuloSolicitado[i].exchange(false);
        algunModuloPorMensaje = algunModuloPorMensaje || porMensajeModulo[i];
    }
    const bool enFoco = JuegoEnFoco();
    EscanearTeclado();
    Latido(enFoco);

    if (!porMensaje && !algunModuloPorMensaje && !enFoco) {
        // Se avisa una sola vez: si el overlay no abre, este renglon separa
        // "no detecto el foco" de "no me llega la tecla".
        if (!avisadoFoco) {
            HWND foco = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(foco, &pid);
            LogF("foco: adelante esta %s (%p, pid %lu), no este proceso (pid %lu). "
                 "Si el overlay no abre por esto, pone foco=0 en Mods\\Compendium\\config.ini",
                 ProcesoDeFoco(pid).c_str(), (void *)foco, pid, GetCurrentProcessId());
            avisadoFoco = true;
        }
        anterior = false;
        for (bool &a : anteriorModulo) {
            a = false;
        }
        return;
    }
    if (!avisado) {
        LogF("tecla de toggle: 0x%02X (VK); cambiala con Mods\\Compendium\\config.ini",
             TeclaToggle());
        avisado = true;
    }

    if (const char *camino = PulsacionNueva(TeclaToggle(), g_teclaEspiada, porMensaje, anterior)) {
        g_algunaTecla = true;
        LogF("1. tecla detectada por %s", camino);
        const unsigned long long delta = MsDesde(ultimoToggle);
        if (delta < 250) {
            LogF("2. descartada: hubo otra hace %llu ms", delta);
        } else {
            AlternarCompendium();
            LogF("2. overlay -> %s", g_visible ? "abierto" : "cerrado");
        }
    }

    for (int i = 0; i < modulos; i++) {
        const Edf6OverlayModule *m = ModuleAt(i);
        const bool conPanel = ModuleHasPanel(i);
        if (m->toggleKey <= 0 || (!m->onToggle && !conPanel)) {
            continue;
        }
        if (const char *camino =
                PulsacionNueva(m->toggleKey, g_teclaModuloEspiada[i], porMensajeModulo[i], anteriorModulo[i])) {
            g_algunaTecla = true;
            const unsigned long long delta = MsDesde(ultimoToggleModulo[i]);
            if (delta < 250) {
                LogF("modulos: tecla de %s por %s descartada, hubo otra hace %llu ms", m->name, camino, delta);
            } else {
                if (conPanel) {
                    AlternarPanelModulo(i);
                }
                if (m->onToggle) {
                    m->onToggle();
                }
                LogF("modulos: tecla de %s por %s%s", m->name, camino,
                     conPanel ? (g_panelModulo == i ? ", panel open" : ", panel closed") : "");
            }
        }
    }
}

static void FrameOverlay(IDXGISwapChain *swap) {
    if (!g_imguiReady) {
        InitImGui(swap);
    }
    AsegurarRenderTarget(swap);
    RevisarSave();
    const bool toast = ToastActivo();
    const bool modulos = AnyModuleWantsDraw();
    if (AlgunPanelVisible() && (!g_imguiReady || !g_rtv)) {
        static bool avisadoSinDibujo = false;
        if (!avisadoSinDibujo) {
            LogF("3. overlay abierto pero NO se puede dibujar: imgui %s, render target %s",
                 g_imguiReady ? "ok" : "sin inicializar", g_rtv ? "ok" : "ausente");
            avisadoSinDibujo = true;
        }
    }
    if (g_imguiReady && (AlgunPanelVisible() || toast || modulos) && g_rtv) {
        static bool avisadoDibujo = false;
        if (!avisadoDibujo) {
            Log("3. primer frame del overlay dibujado");
            avisadoDibujo = true;
        }
        g_enImGui = true;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        g_enImGui = false;
        ImGui::NewFrame();
        if (g_visible) {
            bool abierto = true;
            DrawCompendiumPanel(abierto, g_escala);
            if (!abierto) {
                g_visible = false;
            }
        }
        const int panel = g_panelModulo;
        if (panel >= 0) {
            bool abierto = true;
            DrawModulePanel(panel, g_escala, abierto);
            if (!abierto) {
                int esperado = panel;
                g_panelModulo.compare_exchange_strong(esperado, -1);
            }
        }
        if (toast) {
            DrawToast();
        }
        if (modulos) {
            DrawModules(g_escala);
        }
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

// Los hooks de dibujado van en la vtable del swapchain (un puntero de datos) y
// no como salto al principio de la funcion, y se instalan DESPUES de que el
// overlay de Steam haya parcheado Present (ver HiloInstalar). Hecho antes, Steam
// lee nuestro puntero como si fuera la funcion original y su cadena nos vuelve a
// llamar desde adentro de Present: ciclo infinito y pantalla en blanco.
static void **g_vtbl = nullptr;

static bool LeerBytes(void *dir, unsigned char *out, SIZE_T n) {
    SIZE_T leidos = 0;
    return ReadProcessMemory(GetCurrentProcess(), dir, out, n, &leidos) && leidos == n;
}

// Modulo que contiene una direccion, para el log.
static std::string ModuloDe(const void *dir) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)dir, &m) || !m) {
        return "(memoria fuera de un modulo)";
    }
    char ruta[MAX_PATH] = {0};
    GetModuleFileNameA(m, ruta, MAX_PATH);
    const char *barra = strrchr(ruta, '\\');
    return barra ? barra + 1 : ruta;
}

static bool PonerSlot(int i, void *fn) {
    DWORD viejo = 0;
    if (!VirtualProtect(&g_vtbl[i], sizeof(void *), PAGE_READWRITE, &viejo)) {
        return false;
    }
    g_vtbl[i] = fn;
    VirtualProtect(&g_vtbl[i], sizeof(void *), viejo, &viejo);
    return true;
}

static const int SLOT_PRESENT = 8;
static const int SLOT_RESIZE = 13;
static const int SLOT_PRESENT1 = 22;
static bool g_hayPresent1 = true;

// Present y Present1 son entradas distintas de la vtable: el juego puede usar
// cualquiera. La guarda evita dibujar dos veces si una llama a la otra.
typedef HRESULT(__stdcall *Present1Fn)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static Present1Fn oPresent1 = nullptr;
static thread_local int g_profundidadPresent = 0;

static HRESULT __stdcall hkPresent(IDXGISwapChain *swap, UINT sync, UINT flags) {
    g_frames++;
    if (g_profundidadPresent++ == 0) {
        FrameOverlay(swap);
    }
    HRESULT hr = oPresent(swap, sync, flags);
    g_profundidadPresent--;
    return hr;
}

static HRESULT __stdcall hkPresent1(IDXGISwapChain1 *swap, UINT sync, UINT flags,
                                    const DXGI_PRESENT_PARAMETERS *params) {
    g_frames++;
    if (g_profundidadPresent++ == 0) {
        FrameOverlay(swap);
    }
    HRESULT hr = oPresent1(swap, sync, flags, params);
    g_profundidadPresent--;
    return hr;
}

// Vigilante: si un slot dejo de apuntar a nosotros y ya no llegan frames, alguien
// lo piso sin encadenarnos y se repone. Si los frames siguen llegando no se toca.
static void RevisarSlot(const char *nombre, int slot, void *nuestro, bool parado) {
    void *actual = g_vtbl[slot];
    if (actual == nuestro || !parado) {
        return;
    }
    LogF("slot de %s pisado por %s y sin frames: se repone", nombre, ModuloDe(actual).c_str());
    PonerSlot(slot, nuestro);
}

static DWORD WINAPI HiloVigilante(LPVOID) {
    unsigned long long anterior = g_frames.load();
    for (;;) {
        Sleep(2000);
        const unsigned long long ahora = g_frames.load();
        const bool parado = (ahora == anterior);
        anterior = ahora;
        RevisarSlot("Present", SLOT_PRESENT, (void *)&hkPresent, parado);
        if (g_hayPresent1) {
            RevisarSlot("Present1", SLOT_PRESENT1, (void *)&hkPresent1, parado);
        }
    }
}

static HRESULT __stdcall hkResizeBuffers(IDXGISwapChain *swap, UINT count, UINT w, UINT h,
                                         DXGI_FORMAT fmt, UINT flags) {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    HRESULT hr = oResizeBuffers(swap, count, w, h, fmt, flags);
    AsegurarRenderTarget(swap);
    return hr;
}

// ------------------------------------------------- arranque: vtable prestada

static bool VTableDeSwapChain(void ***vtblOut) {
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "EDF6CompendiumDummy";
    if (!RegisterClassExA(&wc)) {
        Log("RegisterClassEx fallo");
        return false;
    }
    HWND dummy = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                               nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *swap = nullptr;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL nivel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               &nivel, 1, D3D11_SDK_VERSION, &sd, &swap, &dev,
                                               nullptr, &ctx);
    if (FAILED(hr) || !swap) {
        LogF("D3D11CreateDeviceAndSwapChain fallo: 0x%08X", (unsigned)hr);
        DestroyWindow(dummy);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    void **vtbl = *reinterpret_cast<void ***>(swap);
    *vtblOut = vtbl;
    IDXGISwapChain1 *swap1 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), (void **)&swap1)) && swap1) {
        if (*reinterpret_cast<void ***>(swap1) != vtbl) {
            Log("IDXGISwapChain1 tiene otra vtable: no toco el slot de Present1");
            g_hayPresent1 = false;
        }
        swap1->Release();
    }

    swap->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(dummy);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return true;
}

// Instalar los hooks de la vtable ANTES que el overlay de Steam hace que Steam lea
// nuestro puntero como si fuera la funcion original de dxgi, y su cadena termina
// llamandonos de vuelta desde adentro de Present (ciclo infinito, pantalla en
// blanco). Si nos enganchamos despues de que Steam parchee Present, su hook ya
// apunta a su propio trampolin y nosotros solo encadenamos hacia el.
static bool Parcheada(void *dir, const unsigned char *original) {
    unsigned char ahora[16] = {};
    return LeerBytes(dir, ahora, 16) && memcmp(ahora, original, 16) != 0;
}

static void InstalarVTable() {
    oPresent = (PresentFn)g_vtbl[SLOT_PRESENT];
    oResizeBuffers = (ResizeBuffersFn)g_vtbl[SLOT_RESIZE];
    oPresent1 = (Present1Fn)g_vtbl[SLOT_PRESENT1];
    if (!PonerSlot(SLOT_PRESENT, (void *)&hkPresent)) {
        Log("no pude escribir el slot de Present");
        return;
    }
    PonerSlot(SLOT_RESIZE, (void *)&hkResizeBuffers);
    if (g_hayPresent1) {
        PonerSlot(SLOT_PRESENT1, (void *)&hkPresent1);
    }
    Log("hooks instalados en la vtable, esperando el primer frame");
    CreateThread(nullptr, 0, HiloVigilante, nullptr, 0, nullptr);
}

static DWORD WINAPI HiloInstalar(LPVOID) {
    void *funcion = g_vtbl[SLOT_PRESENT];
    unsigned char original[16] = {};
    LeerBytes(funcion, original, 16);

    // Steam se inyecta al arrancar el proceso. Si a los 5 s no esta, no hay a quien esperar.
    for (int i = 0; i < 50 && !GetModuleHandleA("GameOverlayRenderer64.dll"); i++) {
        Sleep(100);
    }
    if (!GetModuleHandleA("GameOverlayRenderer64.dll")) {
        Log("no hay overlay de Steam cargado: engancho ya");
        InstalarVTable();
        return 0;
    }
    Log("overlay de Steam presente: espero a que enganche Present antes de enganchar yo");

    // Steam parchea en el primer frame. Se espera el parche y que quede estable.
    int estables = 0;
    for (int i = 0; i < 200; i++) {  // hasta 20 s
        Sleep(100);
        if (Parcheada(funcion, original)) {
            if (++estables >= 20) {  // 2 s sin cambios despues del parche
                unsigned char b[16] = {};
                LeerBytes(funcion, b, 16);
                LogF("Present parcheado por otro (%02X %02X %02X %02X %02X ...): engancho ahora", b[0],
                     b[1], b[2], b[3], b[4]);
                InstalarVTable();
                return 0;
            }
        } else {
            estables = 0;
        }
    }
    Log("Steam no parcheo Present en 20 s (overlay desactivado?): engancho igual");
    InstalarVTable();
    return 0;
}

static DWORD WINAPI HiloSondeo(LPVOID) {
    for (;;) {
        RevisarToggle();
        Sleep(16);
    }
}

void InitOverlay() {
    CreateThread(nullptr, 0, HiloSondeo, nullptr, 0, nullptr);
    if (!VTableDeSwapChain(&g_vtbl)) {
        return;
    }
    LogF("vtable del swapchain = %p: Present = %p, ResizeBuffers = %p, Present1 = %p",
         (void *)g_vtbl, g_vtbl[SLOT_PRESENT], g_vtbl[SLOT_RESIZE], g_vtbl[SLOT_PRESENT1]);

    // MinHook queda solo para las funciones de user32 del bloqueo de input.
    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize fallo");
        return;
    }
    EngancharInput();
    CreateThread(nullptr, 0, HiloInstalar, nullptr, 0, nullptr);
}
