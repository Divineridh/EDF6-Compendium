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
#include "loadouts.h"
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
static std::atomic<bool> g_loadoutsVisible{false};
static std::atomic<unsigned long long> g_frames{0};
static std::atomic<bool> g_toggleSolicitado{false};
static std::atomic<bool> g_toggleLoadoutsSolicitado{false};

static bool AlgunPanelVisible() {
    return g_visible || g_loadoutsVisible;
}


// ---------------------------------------------------------------- interfaz


static bool g_planListo = false;
static bool g_planSoloWish = false;
static bool g_misionesListas = false;
static bool g_rutaLista = false;

static void DrawRuta();

static const ImVec4 COLOR_WISH(1.00f, 0.45f, 0.60f, 1.00f);
static const ImVec4 COLOR_WISH_OFF(0.38f, 0.38f, 0.38f, 1.00f);

static std::vector<std::string> g_nuevas;
static std::vector<std::string> g_nuevasWish;
static unsigned long long g_toastHasta = 0;

static void AlternarWish(Weapon &w) {
    w.wish = !w.wish;
    SaveWishlist();
    g_planListo = false;
    g_rutaLista = false;
}

static float g_escala = 1.0f;

static float Esc(float v) {
    return v * g_escala;
}

// Alto reservado abajo para el panel de dropeo; las listas de arriba lo restan.
static const float ALTO_DROPS = 330.0f;

static float AltoDrops() {
    const float deseado = Esc(ALTO_DROPS);
    const float tope = ImGui::GetWindowHeight() * 0.40f;
    return deseado < tope ? deseado : tope;
}

// ------------------------------------------------------------- planificador
//
// Da vuelta la pregunta: en vez de "donde sale esta arma", "a que mision me
// conviene ir". Como cada caja elige uniforme del pool de la mision, la chance
// de que te toque algo NUEVO es (faltantes en el pool) / (tamano del pool), y
// el tamano del pool es 1/probabilidad.

struct Plan {
    const Drop *drop;
    int faltantes;
    int pool;
    float porCaja;
    float cajasUna;
    float cajasTodas;
};

// Cajas esperadas para juntar las que faltan. Cada caja sortea uniforme del
// pool, asi que mientras te falten j el promedio es pool/j cajas por acierto:
// el total es pool * (1/j + 1/(j-1) + ... + 1). El clasico coleccionista de
// cupones; con pool/faltantes se subestima muchisimo.
static float CajasParaTodas(int pool, int faltantes) {
    double total = 0.0;
    for (int j = 1; j <= faltantes; j++) {
        total += 1.0 / j;
    }
    return (float)(pool * total);
}

static std::vector<Plan> g_plan;
static int g_planClase = -1;  // -1 = todas las clases

static void CalcularPlan() {
    g_plan.clear();
    const Catalog &cat = GetCatalog();

    for (const Drop &d : GetDrops()) {
        // El pool se cuenta del mismo catalogo que los faltantes. Usar el
        // 1/probabilidad de la hoja mezclaba dos fuentes que no coinciden exacto
        // en las misiones de DLC, y daba chances arriba del 100%.
        //
        // Las cajas sueltan armas de cualquier clase, asi que el pool es siempre
        // el total del rango; el filtro de clase solo acota que contas como
        // faltante. La pregunta que responde es "cuantas de estas cajas me dan
        // algo nuevo PARA esta clase".
        int pool = 0;
        int faltantes = 0;
        int claseIdx = 0;
        for (const ClassData &c : cat.classes) {
            const bool cuenta = (g_planClase == -1 || g_planClase == claseIdx);
            for (const Weapon &w : c.weapons) {
                if (w.tier <= d.tier && d.lo <= w.level && w.level <= d.hi) {
                    pool++;
                    if (!w.owned && cuenta && (!g_planSoloWish || w.wish)) {
                        faltantes++;
                    }
                }
            }
            claseIdx++;
        }
        if (!faltantes || !pool) {
            continue;
        }
        Plan p;
        p.drop = &d;
        p.faltantes = faltantes;
        p.pool = pool;
        p.porCaja = (float)faltantes / (float)pool;
        p.cajasUna = 1.0f / p.porCaja;
        p.cajasTodas = CajasParaTodas(pool, faltantes);
        g_plan.push_back(p);
    }
    std::sort(g_plan.begin(), g_plan.end(),
              [](const Plan &a, const Plan &b) { return a.porCaja > b.porCaja; });
    g_planListo = true;
}

static void DrawPlan() {
    const Catalog &cat = GetCatalog();
    ImGui::TextWrapped(
        "Which mission to farm right now, based on what you are missing. The chance is "
        "the probability that a crate gives you a weapon you do not have yet.");

    ImGui::SetNextItemWidth(Esc(260.0f));
    int previo = g_planClase;
    if (ImGui::BeginCombo("class", g_planClase == -1 ? "all"
                                   : cat.classes[g_planClase].name.c_str())) {
        if (ImGui::Selectable("all", g_planClase == -1)) {
            g_planClase = -1;
        }
        for (int i = 0; i < (int)cat.classes.size(); i++) {
            if (ImGui::Selectable(cat.classes[i].name.c_str(), g_planClase == i)) {
                g_planClase = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("wishlist only", &g_planSoloWish)) {
        g_planListo = false;
    }
    if (g_planSoloWish &&
        ImGui::CollapsingHeader("Route: fewest missions that cover your wishlist")) {
        DrawRuta();
        ImGui::Separator();
    }
    ImGui::SameLine();
    if (ImGui::Button("recalculate") || !g_planListo || previo != g_planClase) {
        CalcularPlan();
    }
    if (g_plan.empty()) {
        const char *motivo = "Nothing left to find in any crate.";
        if (g_planSoloWish) {
            motivo = EnWishlist() == 0
                         ? "Your wishlist is empty: mark weapons with the heart in the class tabs."
                         : "Nothing on your wishlist drops from a crate.";
        }
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "%s", motivo);
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("plan", 8, flags, ImVec2(0.0f, 0.0f))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Difficulty", ImGuiTableColumnFlags_WidthFixed, Esc(120.0f));
    ImGui::TableSetupColumn("Mission", ImGuiTableColumnFlags_WidthFixed, Esc(110.0f));
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(g_planSoloWish ? "Wanted" : "Missing",
                            ImGuiTableColumnFlags_WidthFixed, Esc(100.0f));
    ImGui::TableSetupColumn("Pool", ImGuiTableColumnFlags_WidthFixed, Esc(70.0f));
    ImGui::TableSetupColumn("New per crate", ImGuiTableColumnFlags_WidthFixed, Esc(120.0f));
    ImGui::TableSetupColumn("Crates for 1", ImGuiTableColumnFlags_WidthFixed, Esc(110.0f));
    ImGui::TableSetupColumn("Crates for all", ImGuiTableColumnFlags_WidthFixed, Esc(120.0f));
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < g_plan.size() && i < 200; i++) {
        const Plan &p = g_plan[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s", p.drop->difficulty.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s", p.drop->mission.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", p.drop->missionName.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%d", p.faltantes);
        ImGui::TableSetColumnIndex(4);
        ImGui::TextDisabled("%d", p.pool);
        ImGui::TableSetColumnIndex(5);
        ImGui::TextColored(i < 3 ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) : ImVec4(0.85f, 0.85f, 0.85f, 1.0f),
                           "%.1f%%", p.porCaja * 100.0f);
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%.1f", p.cajasUna);
        ImGui::TableSetColumnIndex(7);
        ImGui::TextDisabled("%.0f", p.cajasTodas);
    }
    ImGui::EndTable();
}

// ------------------------------------------------------------- estrategias
//
// Contenido de la comunidad, no derivado del juego. Por eso cada entrada muestra
// su fuente: los numeros de mision estan verificados contra la tabla, pero los
// rendimientos que afirma cada guia no.

static int g_stratSel = 0;

// Lo que aporta el mod sobre la estrategia: cuantas de TUS faltantes hay en el
// pool de esa mision. Una mision comoda con el pool completo no te sirve.
static void ResumenMision(const std::string &mision) {
    if (mision == "any" || mision.empty()) {
        return;
    }
    const Catalog &cat = GetCatalog();
    bool alguna = false;
    for (const Drop &d : GetDrops()) {
        if (d.mission != mision) {
            continue;
        }
        int pool = 0, faltan = 0;
        for (const ClassData &c : cat.classes) {
            for (const Weapon &w : c.weapons) {
                if (w.tier <= d.tier && d.lo <= w.level && w.level <= d.hi) {
                    pool++;
                    if (!w.owned) faltan++;
                }
            }
        }
        if (!pool) {
            continue;
        }
        if (!alguna) {
            ImGui::Separator();
            ImGui::TextDisabled("Your progress on mission %s:", mision.c_str());
            alguna = true;
        }
        ImGui::SameLine();
        const float porCaja = (float)faltan / (float)pool;
        ImGui::TextColored(faltan ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                           "  %s: %d/%d missing (%.0f%% new per crate)",
                           d.difficulty.c_str(), faltan, pool, porCaja * 100.0f);
    }
}

static void DrawStrats() {
    const std::vector<Strat> &strats = GetStrats();
    if (strats.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "strats.tsv not loaded");
        ImGui::TextDisabled("Expected at Mods\\Compendium\\strats.tsv");
        return;
    }

    ImGui::TextWrapped(
        "Community farming strategies. Mission numbers are checked against the game "
        "data; the yields each guide claims are not verified. Source is shown for each.");
    ImGui::Separator();

    if (g_stratSel >= (int)strats.size()) {
        g_stratSel = 0;
    }

    ImGui::BeginChild("listaStrats", ImVec2(Esc(420.0f), -AltoDrops()), true);
    for (int i = 0; i < (int)strats.size(); i++) {
        const Strat &s = strats[i];
        ImGui::PushID(i);
        if (ImGui::Selectable("##sel", g_stratSel == i, 0, ImVec2(0, Esc(46.0f)))) {
            g_stratSel = i;
        }
        ImGui::SameLine(Esc(8.0f));
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", s.title.c_str());
        if (s.mission == "any") {
            ImGui::TextDisabled("general");
        } else {
            ImGui::TextDisabled("mission %s  ·  %s  ·  %s", s.mission.c_str(),
                                s.difficulty.c_str(), s.className.c_str());
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("detalleStrat", ImVec2(0.0f, -AltoDrops()), false);
    const Strat &s = strats[g_stratSel];
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", s.title.c_str());
    if (s.mission != "any") {
        ImGui::TextDisabled("mission %s  ·  %s  ·  %s", s.mission.c_str(),
                            s.difficulty.c_str(), s.className.c_str());
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", s.body.c_str());
    ResumenMision(s.mission);
    if (!s.source.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("source: %s", s.source.c_str());
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------- misiones
//
// La pregunta al reves de la pestana Farming: no "a que mision voy" sino "de
// esta mision, que me falta". Recorrer las 202 misiones contra las 1560 armas
// en cada frame serian 300k comparaciones, asi que se cachea y se recalcula
// cuando cambia la dificultad o el estado de obtenidas.

static const char *ORDEN_DIF[4] = {"Normal", "Hard", "Hardest", "Inferno"};

struct FilaMision {
    const Drop *drop;
    int falta;
    int pool;
};

static std::vector<FilaMision> g_misiones;
static int g_misDificultad = 3;
static int g_misDifCalc = -1;
static std::string g_misSel;

static void CalcularMisiones() {
    g_misiones.clear();
    const Catalog &cat = GetCatalog();
    const char *dif = ORDEN_DIF[g_misDificultad];
    for (const Drop &d : GetDrops()) {
        if (d.difficulty != dif) {
            continue;
        }
        FilaMision f;
        f.drop = &d;
        f.falta = 0;
        f.pool = 0;
        for (const ClassData &c : cat.classes) {
            for (const Weapon &w : c.weapons) {
                if (w.tier <= d.tier && d.lo <= w.level && w.level <= d.hi) {
                    f.pool++;
                    if (!w.owned) {
                        f.falta++;
                    }
                }
            }
        }
        g_misiones.push_back(f);
    }
    g_misDifCalc = g_misDificultad;
    g_misionesListas = true;
}

// Barras de lo que falta por franja de nivel, con la ventana de la mision
// elegida resaltada. Deja ver si la mision pega donde tenes el hueco o si te
// manda a un tramo que ya completaste.
static void DrawHistograma(float lo, float hi) {
    const int BANDAS = 12;
    int falta[BANDAS] = {0};
    int maxv = 1;
    for (const ClassData &c : GetCatalog().classes) {
        for (const Weapon &w : c.weapons) {
            if (w.owned || !w.farmable) {
                continue;
            }
            int b = w.level / 10;
            if (b >= BANDAS) {
                b = BANDAS - 1;
            }
            falta[b]++;
        }
    }
    for (int i = 0; i < BANDAS; i++) {
        if (falta[i] > maxv) {
            maxv = falta[i];
        }
    }

    ImGui::TextDisabled("Your gaps by level  (green = this mission's window)");
    const float alto = Esc(76.0f);
    const float ancho = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float wb = ancho / BANDAS;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < BANDAS; i++) {
        const bool dentro = (float)(i * 10 + 9) >= lo && (float)(i * 10) <= hi;
        const float h = alto * (float)falta[i] / (float)maxv;
        dl->AddRectFilled(ImVec2(p0.x + i * wb + 1.0f, p0.y + alto - h),
                          ImVec2(p0.x + (i + 1) * wb - 1.0f, p0.y + alto),
                          dentro ? IM_COL32(120, 220, 130, 230) : IM_COL32(95, 100, 115, 200));
        if (falta[i] > 0) {
            char v[8];
            snprintf(v, sizeof(v), "%d", falta[i]);
            const float tw = ImGui::CalcTextSize(v).x;
            dl->AddText(ImVec2(p0.x + i * wb + (wb - tw) * 0.5f, p0.y + alto - h - Esc(15.0f)),
                        IM_COL32(210, 210, 220, 220), v);
        }
    }
    ImGui::Dummy(ImVec2(ancho, alto));
    for (int i = 0; i < BANDAS; i++) {
        char etq[8];
        snprintf(etq, sizeof(etq), "%d", i * 10);
        const float tw = ImGui::CalcTextSize(etq).x;
        dl->AddText(ImVec2(p0.x + i * wb + (wb - tw) * 0.5f, p0.y + alto + Esc(2.0f)),
                    IM_COL32(150, 150, 160, 210), etq);
    }
    ImGui::Dummy(ImVec2(ancho, Esc(18.0f)));
}

static void DrawMisiones() {
    if (GetDrops().empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "missions.tsv not loaded");
        return;
    }
    ImGui::SetNextItemWidth(Esc(180.0f));
    if (ImGui::Combo("##dif", &g_misDificultad, "Normal\0Hard\0Hardest\0Inferno\0")) {
        g_misionesListas = false;
    }
    ImGui::SameLine();
    ImGui::TextWrapped("What is still missing from each mission's own pool.");

    if (!g_misionesListas || g_misDifCalc != g_misDificultad) {
        CalcularMisiones();
    }

    ImGui::BeginChild("listaMisiones", ImVec2(Esc(430.0f), -AltoDrops()), true);
    const float anchoMis = ImGui::GetContentRegionAvail().x;
    for (const FilaMision &f : g_misiones) {
        ImGui::PushID(f.drop);
        char etiqueta[160];
        snprintf(etiqueta, sizeof(etiqueta), "%-10s %s", f.drop->mission.c_str(),
                 f.drop->missionName.c_str());
        if (ImGui::Selectable(etiqueta, g_misSel == f.drop->mission)) {
            g_misSel = f.drop->mission;
        }
        char cuenta[32];
        snprintf(cuenta, sizeof(cuenta), "%d / %d", f.falta, f.pool);
        const float x = anchoMis - ImGui::CalcTextSize(cuenta).x;
        ImGui::SameLine(x > 0.0f ? x : 0.0f);
        ImGui::TextColored(f.falta ? ImVec4(0.75f, 1.0f, 0.75f, 1.0f)
                                   : ImVec4(0.45f, 0.45f, 0.45f, 1.0f),
                           "%s", cuenta);
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("detalleMision", ImVec2(0.0f, -AltoDrops()), false);
    const FilaMision *sel = nullptr;
    for (const FilaMision &f : g_misiones) {
        if (f.drop->mission == g_misSel) {
            sel = &f;
            break;
        }
    }
    if (!sel) {
        ImGui::TextDisabled("Pick a mission on the left.");
    } else {
        const Drop *d = sel->drop;
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s  %s", d->mission.c_str(),
                           d->missionName.c_str());
        ImGui::TextDisabled("%s  |  levels %.0f-%.0f  |  %d of %d missing  |  %.2f%% per crate",
                            d->difficulty.c_str(), d->lo, d->hi, sel->falta, sel->pool,
                            d->chance * 100.0f);
        DrawHistograma(d->lo, d->hi);
        ImGui::Separator();

        const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_BordersOuter;
        if (ImGui::BeginTable("faltanAca", 4, tf, ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Esc(30.0f));
            ImGui::TableSetupColumn("Weapon", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Lv", ImGuiTableColumnFlags_WidthFixed, Esc(46.0f));
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, Esc(130.0f));
            ImGui::TableHeadersRow();
            for (ClassData &c : MutableCatalog().classes) {
                for (Weapon &w : c.weapons) {
                    if (w.owned || w.tier > d->tier || w.level < d->lo || w.level > d->hi) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(w.index);
                    ImGui::PushStyleColor(ImGuiCol_Text, w.wish ? COLOR_WISH : COLOR_WISH_OFF);
                    if (ImGui::Selectable(w.wish ? "\xE2\x99\xA5###wm" : "\xE2\x99\xA1###wm", false,
                                          0, ImVec2(Esc(22.0f), 0.0f))) {
                        AlternarWish(w);
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", w.name.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", w.level);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextDisabled("%s", c.name.c_str());
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ ruta
//
// Cubrimiento de conjuntos, greedy: en cada paso se elige la mision que cubre
// mas cosas de la wishlist que no cubrio ninguna anterior. No da el optimo,
// pero para listas de este tamano la diferencia no importa.

struct Paso {
    const Drop *drop;
    std::vector<std::string> cubre;
};

static std::vector<Paso> g_ruta;

static void CalcularRuta() {
    g_ruta.clear();
    g_rutaLista = true;
    std::vector<const Weapon *> pendientes;
    for (const ClassData &c : GetCatalog().classes) {
        for (const Weapon &w : c.weapons) {
            if (w.wish && !w.owned && w.farmable) {
                pendientes.push_back(&w);
            }
        }
    }
    while (!pendientes.empty() && g_ruta.size() < 10) {
        const Drop *mejor = nullptr;
        std::vector<const Weapon *> mejorCubre;
        for (const Drop &d : GetDrops()) {
            std::vector<const Weapon *> cubre;
            for (const Weapon *w : pendientes) {
                if (w->tier <= d.tier && d.lo <= w->level && w->level <= d.hi) {
                    cubre.push_back(w);
                }
            }
            if (cubre.empty()) {
                continue;
            }
            const bool gana = cubre.size() > mejorCubre.size() ||
                              (mejor && cubre.size() == mejorCubre.size() &&
                               d.chance > mejor->chance);
            if (gana) {
                mejor = &d;
                mejorCubre = cubre;
            }
        }
        if (!mejor) {
            break;
        }
        Paso paso;
        paso.drop = mejor;
        for (const Weapon *w : mejorCubre) {
            paso.cubre.push_back(w->name);
            pendientes.erase(std::remove(pendientes.begin(), pendientes.end(), w),
                             pendientes.end());
        }
        g_ruta.push_back(paso);
    }
}

static void DrawRuta() {
    if (!g_rutaLista) {
        CalcularRuta();
    }
    if (g_ruta.empty()) {
        ImGui::TextDisabled("Nothing farmable on your wishlist right now.");
        return;
    }
    for (size_t i = 0; i < g_ruta.size(); i++) {
        const Paso &paso = g_ruta[i];
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%d.", (int)i + 1);
        ImGui::SameLine();
        ImGui::Text("%s %s", paso.drop->mission.c_str(), paso.drop->missionName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s, %.2f%% per crate)", paso.drop->difficulty.c_str(),
                            paso.drop->chance * 100.0f);
        for (const std::string &nombre : paso.cubre) {
            ImGui::TextColored(COLOR_WISH, "      \xE2\x99\xA5 %s", nombre.c_str());
        }
    }
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
    g_planListo = false;
    g_misionesListas = false;
    g_rutaLista = false;
}

static bool ToastActivo() {
    return GetTickCount64() < g_toastHasta && !g_nuevasWish.empty();
}

static void DrawToast() {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - Esc(28.0f), vp->WorkPos.y + Esc(28.0f)),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##toastWish", nullptr, flags)) {
        ImGui::TextColored(COLOR_WISH, "\xE2\x99\xA5 Wishlisted item dropped!");
        for (const std::string &nombre : g_nuevasWish) {
            ImGui::Text("   %s", nombre.c_str());
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
static std::atomic<bool> g_teclaLoadoutsEspiada{false};
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
    if (vk == TeclaLoadouts()) {
        g_teclaLoadoutsEspiada = (r & 0x8000) != 0;
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
        const int vkLoadouts = TeclaLoadouts();
        if (vkLoadouts > 0 && vkLoadouts < 256) {
            g_teclaLoadoutsEspiada = (estado[vkLoadouts] & 0x80) != 0;
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
    if (msg == WM_KEYDOWN && wp == (WPARAM)TeclaLoadouts()) {
        g_toggleLoadoutsSolicitado = true;
        return 0;
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

static void AlternarPanel(std::atomic<bool> &propio, std::atomic<bool> &otro) {
    const bool yaBloqueaba = AlgunPanelVisible();
    propio = !propio;
    if (propio) {
        otro = false;
        if (!yaBloqueaba && oGetCursorPos) {
            oGetCursorPos(&g_cursorCongelado);
        }
    }
}

static void RevisarToggle() {
    static bool anterior = false;
    static bool anteriorLoadouts = false;
    static bool avisado = false;
    static bool avisadoFoco = false;
    static unsigned long long ultimoToggle = 0;
    static unsigned long long ultimoToggleLoadouts = 0;

    // Un WM_KEYDOWN solo llega si la ventana tiene el foco de teclado de verdad,
    // asi que ese camino no pasa por el filtro. En las maquinas donde
    // GetForegroundWindow miente, es el unico que queda.
    const bool porMensaje = g_toggleSolicitado.exchange(false);
    const bool porMensajeLoadouts = g_toggleLoadoutsSolicitado.exchange(false);
    const bool enFoco = JuegoEnFoco();
    EscanearTeclado();
    Latido(enFoco);

    if (!porMensaje && !porMensajeLoadouts && !enFoco) {
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
        anteriorLoadouts = false;
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
            AlternarPanel(g_visible, g_loadoutsVisible);
            LogF("2. overlay -> %s", g_visible ? "abierto" : "cerrado");
        }
    }

    if (const char *camino = PulsacionNueva(TeclaLoadouts(), g_teclaLoadoutsEspiada,
                                            porMensajeLoadouts, anteriorLoadouts)) {
        g_algunaTecla = true;
        const unsigned long long delta = MsDesde(ultimoToggleLoadouts);
        if (delta < 250) {
            LogF("loadouts: tecla por %s descartada, hubo otra hace %llu ms", camino, delta);
        } else {
            AlternarPanel(g_loadoutsVisible, g_visible);
            LogF("loadouts: tecla por %s, panel -> %s", camino, g_loadoutsVisible ? "abierto" : "cerrado");
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
    if (AlgunPanelVisible() && (!g_imguiReady || !g_rtv)) {
        static bool avisadoSinDibujo = false;
        if (!avisadoSinDibujo) {
            LogF("3. overlay abierto pero NO se puede dibujar: imgui %s, render target %s",
                 g_imguiReady ? "ok" : "sin inicializar", g_rtv ? "ok" : "ausente");
            avisadoSinDibujo = true;
        }
    }
    if (g_imguiReady && (AlgunPanelVisible() || toast) && g_rtv) {
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
        if (g_loadoutsVisible) {
            bool abierto = true;
            DrawLoadoutsPanel(abierto, g_escala);
            if (!abierto) {
                g_loadoutsVisible = false;
            }
        }
        if (toast) {
            DrawToast();
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
