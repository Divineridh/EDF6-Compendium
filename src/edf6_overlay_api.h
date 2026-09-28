#pragma once

#include <stddef.h>
#include <stdint.h>

#define EDF6_OVERLAY_API_VERSION 3
#define EDF6_OVERLAY_HOST_DLL "EDF6Compendium.dll"
#define EDF6_OVERLAY_REGISTER "Edf6Overlay_Register"

enum Edf6OverlayFont {
    EDF6_FONT_REGULAR = 0,
    EDF6_FONT_SEMIBOLD = 1,
    EDF6_FONT_BOLD = 2,
    EDF6_FONT_LABEL = 3,
    EDF6_FONT_MONO = 4,
};

/* Strings are UTF-8 and live for the whole process. className is "Ranger", "Wing Diver",
   "Air Raider" or "Fencer". starred means every stat is upgraded to its cap. owned and starred
   follow the save, so they change after a mission. */
struct Edf6Weapon {
    int index;
    const char *name;
    const char *className;
    const char *category;
    int level;
    int owned;
    int starred;
};

typedef void *(*Edf6ImguiAllocFn)(size_t size, void *userData);
typedef void (*Edf6ImguiFreeFn)(void *ptr, void *userData);

/* Colors are 0xRRGGBBAA. Sizes and positions are screen pixels. Fields are only appended:
   everything after a "version N" mark exists when host->version >= N. */
struct Edf6OverlayHost {
    int version;
    void (*log)(const char *message);
    float (*scale)(void);
    void (*screenSize)(float *width, float *height);
    void (*fillRect)(float x0, float y0, float x1, float y1, uint32_t rgba);
    void (*strokeRect)(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);
    void (*text)(float x, float y, float size, uint32_t rgba, const char *utf8);
    void (*textSize)(float size, const char *utf8, float *width, float *height);
    /* version 2 */
    void (*textEx)(float x, float y, float size, uint32_t rgba, int font, float spacing, const char *utf8);
    void (*textExSize)(float size, int font, float spacing, const char *utf8, float *width, float *height);
    /* version 3 */
    void *(*imguiContext)(void);
    void (*imguiAllocators)(Edf6ImguiAllocFn *alloc, Edf6ImguiFreeFn *free, void **userData);
    void *(*imguiFont)(int font);
    int (*weaponCount)(void);
    int (*weapon)(int index, struct Edf6Weapon *out);
};

/* Only meaningful after including imgui.h. The host compares it with its own before accepting a
   module with a panel, so a module built against another imgui is refused instead of crashing. */
#define EDF6_IMGUI_LAYOUT                                                                                 \
    ((uint32_t)(sizeof(ImGuiIO) * 1000003u + sizeof(ImGuiStyle) * 10007u + sizeof(ImDrawVert) * 101u + \
                sizeof(ImDrawIdx) * 11u + sizeof(ImVec4)))

/* The module struct must outlive the process. onToggle runs on the host's input thread;
   wantsDraw, draw and panel run on the render thread. host->log, weaponCount and weapon work from
   any thread at any time; every other host function is only valid inside draw or panel.
   weaponCount returns 0 until the host has loaded its catalog. A module registered with version N
   needs a host with API version N or newer.

   Panels (version 3): a module with a panel gets a window of its own. Its toggle key opens and
   closes it, only one panel is open at a time (the Compendium's included), and while it is open
   the host blocks the game's input and shows the cursor. panel runs inside the host's imgui
   frame: call ImGui::SetAllocatorFunctions and ImGui::SetCurrentContext with the host's before
   any imgui call, and set *open to 0 to close. imguiVersion and imguiLayout must be
   IMGUI_VERSION_NUM and EDF6_IMGUI_LAYOUT from the imgui the module was built with. */
struct Edf6OverlayModule {
    int version;
    const char *name;
    int toggleKey;
    void (*onToggle)(void);
    int (*wantsDraw)(void);
    void (*draw)(const struct Edf6OverlayHost *host);
    /* version 3 */
    void (*panel)(const struct Edf6OverlayHost *host, int *open);
    int imguiVersion;
    uint32_t imguiLayout;
};

typedef int (*Edf6OverlayRegisterFn)(const struct Edf6OverlayModule *module, const struct Edf6OverlayHost **host);
