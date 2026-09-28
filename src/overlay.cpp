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

// The save is checked every two seconds, not every frame: plenty to notice when
// you come back from a mission, and it has to run with the overlay closed so the
// notice shows up without opening it.
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

// ------------------------------------------------------------ input blocking
//
// EDF6 doesn't read the mouse through window messages: it imports GetKeyState,
// GetKeyboardState and GetCursorPos, so it queries the state directly and
// skips the WndProc. To keep the overlay from triggering game actions, those
// three have to lie to it.
//
// The catch is that imgui's Win32 backend uses the same functions. So the block
// is lifted during our NewFrame: inside that window the calls are ours and go
// through; outside it they are the game's and get neutralized.

typedef SHORT(WINAPI *GetKeyStateFn)(int);
typedef BOOL(WINAPI *GetKeyboardStateFn)(PBYTE);
typedef BOOL(WINAPI *GetCursorPosFn)(LPPOINT);

static GetKeyStateFn oGetKeyState = nullptr;
static GetKeyboardStateFn oGetKeyboardState = nullptr;
static GetCursorPosFn oGetCursorPos = nullptr;

static bool g_enImGui = false;
static POINT g_cursorCongelado = {0, 0};

// Third input path, and the most reliable one: these two functions were already
// hooked to mute the keyboard for the game, and GetKeyboardState returns the
// state of all 256 keys. Reading it there uses exactly the same data the game
// uses: if the game responds to the keyboard, this responds too. No new API and
// no global hook needed.
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
            LogF("couldn't find %s", o.nombre);
            continue;
        }
        if (MH_CreateHook(dir, o.reemplazo, o.original) != MH_OK ||
            MH_EnableHook(dir) != MH_OK) {
            LogF("couldn't hook %s", o.nombre);
        }
    }
    Log("input hooks installed");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // There are two input paths: this message and the GetAsyncKeyState poll in
    // RevisarToggle(). On some machines the poll never sees the key, so the
    // request is noted here and handled there; a cooldown avoids a double toggle
    // when both arrive.
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


// The render target is lost on every ResizeBuffers and can fail on the first
// frame. It used to be created once at init: if that failed, the overlay stayed
// silent forever without saying anything. Now it is retried every frame.
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
        Log("no render target yet; retrying every frame");
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
            Log("couldn't get the device from the swapchain");
            avisado = true;
        }
        return;
    }
    g_imguiCreado = true;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc = {};
    swap->GetDesc(&desc);
    g_window = desc.OutputWindow;
    LogF("swapchain window = %p", (void *)g_window);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    // imgui's default font is ProggyClean: 13px and ASCII only, so at 1080p it
    // is unreadable and can't draw the star. Segoe UI is loaded instead, with
    // Segoe UI Symbol merged in just for U+2605, the mark the game uses for
    // maxed weapons.
    ImFont *base = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    if (base) {
        static const ImWchar rangoEstrella[] = {0x2605, 0x2606, 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 16.0f, &cfg,
                                          rangoEstrella)) {
            Log("couldn't merge the star symbol");
        }
    } else {
        // Without the system font, imgui's own: it looks worse but works.
        Log("couldn't load segoeui.ttf, using the default font");
    }
    ui::LoadFonts();

    // io.FontGlobalScale moved to style.FontScaleMain in imgui 1.92.
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

// The WM_KEYDOWN toggle depends on the game dispatching messages to the window,
// and EDF6 processes them in its own PeekMessage loop. That's why the key is also
// polled every frame: GetAsyncKeyState doesn't go through the queue and isn't
// hooked, so it works anyway.
// The check is by PROCESS, not by window handle: the game may have a different
// window focused than the swapchain's depending on the display mode or injected
// layers, and comparing handles left the toggle dead forever. The filter is only
// meant to keep the key from firing while you are in another application.

// Name of the executable in front. Only for the log: when the filter fails,
// this is the only thing that tells "the game wasn't in front" apart from "this
// machine doesn't report it".
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

// How long to wait before giving up on the filter.
static const ULONGLONG ESPERA_FOCO_MS = 8000;

// On a tester's machine GetForegroundWindow never returned a game window, so the
// filter was false every frame and the overlay never opened. Since the filter is
// a convenience and not a need, if this process isn't seen in front even once in
// the first seconds of play, the data is assumed useless on this machine and the
// filter stops being checked.
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
        LogF("the focus filter doesn't work on this machine: %s (pid %lu) is in front and this "
             "process (pid %lu) never was. Turning it off; the key works anyway",
             ProcesoDeFoco(pid).c_str(), pid, GetCurrentProcessId());
        filtro = false;
        return true;
    }
    return false;
}

static bool g_algunaTecla = false;

// Sweeps every key code while none has been seen. It answers what no other log
// line does: whether GetAsyncKeyState works at all in this process. If it never
// prints, the keyboard doesn't arrive that way and the configured key doesn't
// matter. Only the first key seen is logged.
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
        Log("0b. the key arrives through the game's own keyboard read");
        return;
    }
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            visto = true;
            LogF("0b. the keyboard DOES respond: first key seen 0x%02X (configured one is 0x%02X)",
                 vk, TeclaToggle());
            return;
        }
    }
}

// Heartbeat while no key has been detected: proves the loop runs and that focus
// isn't holding it back. Without it, "nothing happens" and "the key doesn't
// arrive" look the same in the log.
static void Latido(bool enFoco) {
    static unsigned long long proximo = 0;
    const unsigned long long t = GetTickCount64();
    if (g_algunaTecla || t < proximo) {
        return;
    }
    static unsigned long long framesPrevios = 0;
    const unsigned long long frames = g_frames;
    if (proximo) {
        LogF("0. polling: %llu frames in the last 10s%s. Key 0x%02X not detected, "
             "the game %s the keyboard",
             frames - framesPrevios,
             (frames == framesPrevios && g_imguiReady) ? "  <-- WE ARE NOT BEING CALLED TO DRAW" : "",
             TeclaToggle(), g_juegoLeeTeclado ? "DOES read" : "does NOT read");
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
    return !porSondeo ? "window message" : (porApi ? "direct poll" : "the game's own read");
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

    // A WM_KEYDOWN only arrives if the window really has keyboard focus, so that
    // path skips the filter. On machines where GetForegroundWindow lies, it's the
    // only one left.
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
        // Logged only once: if the overlay doesn't open, this line separates
        // "focus not detected" from "the key doesn't arrive".
        if (!avisadoFoco) {
            HWND foco = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(foco, &pid);
            LogF("focus: %s (%p, pid %lu) is in front, not this process (pid %lu). "
                 "If the overlay doesn't open because of this, set foco=0 in Mods\\Compendium\\config.ini",
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
        LogF("toggle key: 0x%02X (VK); change it in Mods\\Compendium\\config.ini",
             TeclaToggle());
        avisado = true;
    }

    if (const char *camino = PulsacionNueva(TeclaToggle(), g_teclaEspiada, porMensaje, anterior)) {
        g_algunaTecla = true;
        LogF("1. key detected by %s", camino);
        const unsigned long long delta = MsDesde(ultimoToggle);
        if (delta < 250) {
            LogF("2. descartada: hubo otra hace %llu ms", delta);
        } else {
            AlternarCompendium();
            LogF("2. overlay -> %s", g_visible ? "open" : "closed");
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
                LogF("modules: %s key by %s ignored, another one %llu ms ago", m->name, camino, delta);
            } else {
                if (conPanel) {
                    AlternarPanelModulo(i);
                }
                if (m->onToggle) {
                    m->onToggle();
                }
                LogF("modules: %s key by %s%s", m->name, camino,
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
            LogF("3. overlay open but it CAN'T draw: imgui %s, render target %s",
                 g_imguiReady ? "ok" : "not initialized", g_rtv ? "ok" : "missing");
            avisadoSinDibujo = true;
        }
    }
    if (g_imguiReady && (AlgunPanelVisible() || toast || modulos) && g_rtv) {
        static bool avisadoDibujo = false;
        if (!avisadoDibujo) {
            Log("3. first overlay frame drawn");
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

// The drawing hooks go in the swapchain's vtable (a data pointer), not as a jump
// at the start of the function, and they are installed AFTER the Steam overlay
// has patched Present (see HiloInstalar). Done before, Steam reads our pointer as
// if it were the original function and its chain calls us again from inside
// Present: infinite loop and a white screen.
static void **g_vtbl = nullptr;

static bool LeerBytes(void *dir, unsigned char *out, SIZE_T n) {
    SIZE_T leidos = 0;
    return ReadProcessMemory(GetCurrentProcess(), dir, out, n, &leidos) && leidos == n;
}

// Module that contains an address, for the log.
static std::string ModuloDe(const void *dir) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)dir, &m) || !m) {
        return "(memory outside any module)";
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

// Present and Present1 are different vtable entries: the game may use either.
// The guard avoids drawing twice if one calls the other.
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

// Watchdog: if a slot stopped pointing at us and frames stopped arriving, someone
// overwrote it without chaining us and it is restored. If frames keep coming it's left alone.
static void RevisarSlot(const char *nombre, int slot, void *nuestro, bool parado) {
    void *actual = g_vtbl[slot];
    if (actual == nuestro || !parado) {
        return;
    }
    LogF("%s slot overwritten by %s and no frames: restoring it", nombre, ModuloDe(actual).c_str());
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
            Log("IDXGISwapChain1 has another vtable: leaving the Present1 slot alone");
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

// Installing the vtable hooks BEFORE the Steam overlay makes Steam read our
// pointer as if it were dxgi's original function, and its chain ends up calling
// us back from inside Present (infinite loop, white screen). Hooking after Steam
// patches Present, its hook already points at its own trampoline and we just
// chain to it.
static bool Parcheada(void *dir, const unsigned char *original) {
    unsigned char ahora[16] = {};
    return LeerBytes(dir, ahora, 16) && memcmp(ahora, original, 16) != 0;
}

static void InstalarVTable() {
    oPresent = (PresentFn)g_vtbl[SLOT_PRESENT];
    oResizeBuffers = (ResizeBuffersFn)g_vtbl[SLOT_RESIZE];
    oPresent1 = (Present1Fn)g_vtbl[SLOT_PRESENT1];
    if (!PonerSlot(SLOT_PRESENT, (void *)&hkPresent)) {
        Log("couldn't write the Present slot");
        return;
    }
    PonerSlot(SLOT_RESIZE, (void *)&hkResizeBuffers);
    if (g_hayPresent1) {
        PonerSlot(SLOT_PRESENT1, (void *)&hkPresent1);
    }
    Log("hooks installed in the vtable, waiting for the first frame");
    CreateThread(nullptr, 0, HiloVigilante, nullptr, 0, nullptr);
}

static DWORD WINAPI HiloInstalar(LPVOID) {
    void *funcion = g_vtbl[SLOT_PRESENT];
    unsigned char original[16] = {};
    LeerBytes(funcion, original, 16);

    // Steam injects itself at process start. If it isn't there after 5 s, there's nobody to wait for.
    for (int i = 0; i < 50 && !GetModuleHandleA("GameOverlayRenderer64.dll"); i++) {
        Sleep(100);
    }
    if (!GetModuleHandleA("GameOverlayRenderer64.dll")) {
        Log("no Steam overlay loaded: hooking now");
        InstalarVTable();
        return 0;
    }
    Log("Steam overlay present: waiting for it to hook Present before hooking");

    // Steam patches on the first frame. Wait for the patch and for it to settle.
    int estables = 0;
    for (int i = 0; i < 200; i++) {  // up to 20 s
        Sleep(100);
        if (Parcheada(funcion, original)) {
            if (++estables >= 20) {  // 2 s without changes after the patch
                unsigned char b[16] = {};
                LeerBytes(funcion, b, 16);
                LogF("Present patched by someone else (%02X %02X %02X %02X %02X ...): hooking now", b[0],
                     b[1], b[2], b[3], b[4]);
                InstalarVTable();
                return 0;
            }
        } else {
            estables = 0;
        }
    }
    Log("Steam didn't patch Present within 20 s (overlay disabled?): hooking anyway");
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
    LogF("swapchain vtable = %p: Present = %p, ResizeBuffers = %p, Present1 = %p",
         (void *)g_vtbl, g_vtbl[SLOT_PRESENT], g_vtbl[SLOT_RESIZE], g_vtbl[SLOT_PRESENT1]);

    // MinHook is only left for the user32 functions of the input block.
    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize fallo");
        return;
    }
    EngancharInput();
    CreateThread(nullptr, 0, HiloInstalar, nullptr, 0, nullptr);
}
