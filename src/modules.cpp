#include <windows.h>

#include <atomic>
#include <cfloat>
#include <mutex>

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

const Edf6OverlayHost g_host = {
    EDF6_OVERLAY_API_VERSION, &HostLog,  &HostScale, &HostScreenSize, &HostFillRect,
    &HostStrokeRect,          &HostText, &HostTextSize,
};

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
    if (module->version != EDF6_OVERLAY_API_VERSION) {
        LogF("modulos: %s pide la API %d y el Compendium tiene la %d; no lo registro",
             module->name ? module->name : "?", module->version, EDF6_OVERLAY_API_VERSION);
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
    LogF("modulos: registrado %s, tecla 0x%02X", module->name ? module->name : "?", module->toggleKey);
    return 1;
}
