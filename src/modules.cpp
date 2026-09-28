#include <windows.h>

#include <atomic>
#include <cfloat>
#include <mutex>
#include <vector>

#include "imgui.h"

#include "compendium.h"
#include "modules.h"
#include "ui_kit.h"

namespace {

const Edf6OverlayModule *g_modules[kMaxModules] = {};
std::atomic<int> g_count{0};
std::mutex g_registerMutex;
float g_scale = 1.0f;

ImU32 FromRgba(uint32_t rgba) {
    return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
}

ImFont *ModuleFont() {
    return ui::FontOr(ui::g_fontSemi);
}

void HostLog(const char *message) {
    Log(message);
}

float HostScale() {
    return g_scale;
}

void HostScreenSize(float *width, float *height) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    *width = size.x;
    *height = size.y;
}

void HostFillRect(float x0, float y0, float x1, float y1, uint32_t rgba) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), FromRgba(rgba));
}

void HostStrokeRect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness) {
    ImGui::GetBackgroundDrawList()->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), FromRgba(rgba), 0.0f, thickness);
}

void HostText(float x, float y, float size, uint32_t rgba, const char *utf8) {
    ImGui::GetBackgroundDrawList()->AddText(ModuleFont(), size, ImVec2(x, y), FromRgba(rgba), utf8);
}

void HostTextSize(float size, const char *utf8, float *width, float *height) {
    const ImVec2 s = ModuleFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, utf8);
    *width = s.x;
    *height = s.y;
}

ImFont *FontById(int font) {
    switch (font) {
    case EDF6_FONT_SEMIBOLD:
        return ui::FontOr(ui::g_fontSemi);
    case EDF6_FONT_BOLD:
        return ui::FontOr(ui::g_fontBold);
    case EDF6_FONT_LABEL:
        return ui::FontOr(ui::g_fontLabel);
    case EDF6_FONT_MONO:
        return ui::FontOr(ui::g_fontMono);
    default:
        return ui::FontOr(nullptr);
    }
}

int Utf8Length(unsigned char c) {
    return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
}

void HostTextEx(float x, float y, float size, uint32_t rgba, int font, float spacing, const char *utf8) {
    ImFont *f = FontById(font);
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (spacing == 0.0f) {
        dl->AddText(f, size, ImVec2(x, y), FromRgba(rgba), utf8);
        return;
    }
    for (const char *c = utf8; *c;) {
        const int n = Utf8Length((unsigned char)*c);
        dl->AddText(f, size, ImVec2(x, y), FromRgba(rgba), c, c + n);
        x += f->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + n).x + spacing;
        c += n;
    }
}

void HostTextExSize(float size, int font, float spacing, const char *utf8, float *width, float *height) {
    const ImVec2 s = FontById(font)->CalcTextSizeA(size, FLT_MAX, 0.0f, utf8);
    int glyphs = 0;
    for (const char *c = utf8; *c; c += Utf8Length((unsigned char)*c)) {
        glyphs++;
    }
    *width = s.x + (glyphs > 1 ? spacing * (glyphs - 1) : 0.0f);
    *height = s.y;
}

void *HostImguiContext() {
    return ImGui::GetCurrentContext();
}

void HostImguiAllocators(Edf6ImguiAllocFn *alloc, Edf6ImguiFreeFn *free, void **userData) {
    ImGuiMemAllocFunc a = nullptr;
    ImGuiMemFreeFunc f = nullptr;
    ImGui::GetAllocatorFunctions(&a, &f, userData);
    *alloc = a;
    *free = f;
}

void *HostImguiFont(int font) {
    return FontById(font);
}

// Filled once by MarkCatalogReady and read-only afterwards, so modules can query it from any
// thread without locking.
std::atomic<bool> g_catalogReady{false};
std::vector<const Weapon *> g_weaponByIndex;
std::vector<const char *> g_classOfIndex;

int HostWeaponCount() {
    return g_catalogReady.load(std::memory_order_acquire) ? (int)g_weaponByIndex.size() : 0;
}

int HostWeapon(int index, Edf6Weapon *out) {
    if (index < 0 || index >= HostWeaponCount() || !g_weaponByIndex[index] || !out) {
        return 0;
    }
    const Weapon *w = g_weaponByIndex[index];
    out->index = w->index;
    out->name = w->name.c_str();
    out->className = g_classOfIndex[index];
    out->category = w->categoryName.c_str();
    out->level = w->level;
    out->owned = w->owned ? 1 : 0;
    out->starred = w->starred ? 1 : 0;
    return 1;
}

const Edf6OverlayHost g_host = {
    EDF6_OVERLAY_API_VERSION, &HostLog,          &HostScale,           &HostScreenSize, &HostFillRect,
    &HostStrokeRect,          &HostText,         &HostTextSize,        &HostTextEx,     &HostTextExSize,
    &HostImguiContext,        &HostImguiAllocators, &HostImguiFont,    &HostWeaponCount, &HostWeapon,
};

bool HasPanel(const Edf6OverlayModule *m) {
    return m->version >= 3 && m->panel;
}

}

void MarkCatalogReady() {
    int maxIndex = -1;
    for (const ClassData &c : GetCatalog().classes) {
        for (const Weapon &w : c.weapons) {
            maxIndex = w.index > maxIndex ? w.index : maxIndex;
        }
    }
    g_weaponByIndex.assign(maxIndex + 1, nullptr);
    g_classOfIndex.assign(maxIndex + 1, "");
    for (const ClassData &c : GetCatalog().classes) {
        for (const Weapon &w : c.weapons) {
            g_weaponByIndex[w.index] = &w;
            g_classOfIndex[w.index] = c.name.c_str();
        }
    }
    g_catalogReady.store(true, std::memory_order_release);
}

bool ModuleHasPanel(int index) {
    return HasPanel(g_modules[index]);
}

void DrawModulePanel(int index, float scale, bool &open) {
    g_scale = scale;
    int stillOpen = 1;
    g_modules[index]->panel(&g_host, &stillOpen);
    open = stillOpen != 0;
}

int ModuleCount() {
    return g_count.load(std::memory_order_acquire);
}

const Edf6OverlayModule *ModuleAt(int index) {
    return g_modules[index];
}

bool AnyModuleWantsDraw() {
    const int count = ModuleCount();
    for (int i = 0; i < count; i++) {
        if (g_modules[i]->wantsDraw && g_modules[i]->wantsDraw()) {
            return true;
        }
    }
    return false;
}

void DrawModules(float scale) {
    g_scale = scale;
    const int count = ModuleCount();
    for (int i = 0; i < count; i++) {
        const Edf6OverlayModule *m = g_modules[i];
        if (m->draw && m->wantsDraw && m->wantsDraw()) {
            m->draw(&g_host);
        }
    }
}

extern "C" __declspec(dllexport) int Edf6Overlay_Register(const Edf6OverlayModule *module,
                                                          const Edf6OverlayHost **host) {
    if (!module || !host) {
        return 0;
    }
    if (module->version < 1 || module->version > EDF6_OVERLAY_API_VERSION) {
        LogF("modulos: %s pide la API %d y el Compendium llega hasta la %d; no lo registro",
             module->name ? module->name : "?", module->version, EDF6_OVERLAY_API_VERSION);
        return 0;
    }
    if (HasPanel(module) &&
        (module->imguiVersion != IMGUI_VERSION_NUM || module->imguiLayout != EDF6_IMGUI_LAYOUT)) {
        LogF("modulos: %s was built with imgui %d (layout %08X) and the Compendium with %d (layout %08X); "
             "not registered",
             module->name ? module->name : "?", module->imguiVersion, module->imguiLayout, IMGUI_VERSION_NUM,
             EDF6_IMGUI_LAYOUT);
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_registerMutex);
    const int count = g_count.load(std::memory_order_relaxed);
    if (count >= kMaxModules) {
        LogF("modulos: sin lugar para %s", module->name ? module->name : "?");
        return 0;
    }
    g_modules[count] = module;
    g_count.store(count + 1, std::memory_order_release);
    *host = &g_host;
    LogF("modulos: registrado %s (API %d), tecla 0x%02X%s", module->name ? module->name : "?", module->version,
         module->toggleKey, HasPanel(module) ? ", con panel" : "");
    return 1;
}
