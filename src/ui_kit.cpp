#include <windows.h>

#include <cfloat>
#include <cstdio>

#include "compendium.h"
#include "ui_kit.h"

namespace ui {

ImFont *g_fontLabel = nullptr;
ImFont *g_fontBold = nullptr;
ImFont *g_fontSemi = nullptr;
ImFont *g_fontMono = nullptr;

namespace {

float g_k = 1.0f;

int Utf8Length(unsigned char c) {
    return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
}

}

void LoadFonts() {
    struct {
        const char *path;
        ImFont **target;
    } fonts[] = {
        {"C:\\Windows\\Fonts\\bahnschrift.ttf", &g_fontLabel},
        {"C:\\Windows\\Fonts\\segoeuib.ttf", &g_fontBold},
        {"C:\\Windows\\Fonts\\seguisb.ttf", &g_fontSemi},
        {"C:\\Windows\\Fonts\\consola.ttf", &g_fontMono},
    };
    ImGuiIO &io = ImGui::GetIO();
    for (auto &f : fonts) {
        if (GetFileAttributesA(f.path) == INVALID_FILE_ATTRIBUTES) {
            LogF("ui: no esta %s, uso la fuente por defecto", f.path);
            continue;
        }
        *f.target = io.Fonts->AddFontFromFileTTF(f.path, 16.0f);
    }
}

void SetScale(float k) {
    g_k = k;
}

float D(float px) {
    return px * g_k;
}

float FontBase(float px) {
    return D(px) / ImGui::GetStyle().FontScaleMain;
}

ImU32 Rgb(uint32_t hex, float alpha) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, (int)(alpha * 255.0f));
}

ImFont *FontOr(ImFont *f) {
    return f ? f : ImGui::GetFont();
}

ImVec2 Measure(ImFont *f, float px, const char *t, const char *end) {
    return FontOr(f)->CalcTextSizeA(D(px), FLT_MAX, 0.0f, t, end);
}

void PaintText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const char *t) {
    dl->AddText(FontOr(f), D(px), p, Rgb(col), t);
}

float SpacedText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const char *t, float spacing) {
    const float start = p.x;
    for (const char *c = t; *c;) {
        const int n = Utf8Length((unsigned char)*c);
        dl->AddText(FontOr(f), D(px), p, Rgb(col), c, c + n);
        p.x += Measure(f, px, c, c + n).x + D(spacing);
        c += n;
    }
    return p.x - start;
}

std::string Upper(const char *s) {
    std::string out = s;
    for (char &ch : out) {
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
    }
    return out;
}

std::string FitText(ImFont *f, float px, const std::string &s, float maxWidth, bool &cut) {
    cut = Measure(f, px, s.c_str()).x > maxWidth;
    if (!cut) {
        return s;
    }
    const std::string ellipsis = "\xE2\x80\xA6";
    std::string t = s;
    while (!t.empty()) {
        size_t at = t.size() - 1;
        while (at > 0 && ((unsigned char)t[at] & 0xC0) == 0x80) {
            at--;
        }
        t.erase(at);
        if (Measure(f, px, (t + ellipsis).c_str()).x <= maxWidth) {
            break;
        }
    }
    return t + ellipsis;
}

void FittedText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const std::string &s, float maxWidth) {
    bool cut = false;
    const std::string shown = FitText(f, px, s, maxWidth, cut);
    PaintText(dl, f, px, p, col, shown.c_str());
    const ImVec2 size = Measure(f, px, shown.c_str());
    if (cut && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + size.x, p.y + size.y))) {
        ImGui::SetTooltip("%s", s.c_str());
    }
}

float KeyHint(ImDrawList *dl, ImVec2 p, const char *key, uint32_t text, uint32_t border) {
    const ImVec2 size = Measure(g_fontLabel, 12.0f, key);
    const float w = size.x + D(12.0f);
    const float h = D(22.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), Rgb(border), 0.0f, D(1.0f));
    PaintText(dl, g_fontLabel, 12.0f, ImVec2(p.x + D(6.0f), p.y + (h - size.y) * 0.5f), text, key);
    return w;
}

void DashedRect(ImDrawList *dl, ImVec2 a, ImVec2 b, uint32_t col) {
    const float dash = D(5.0f);
    const float gap = D(4.0f);
    const float t = D(1.0f);
    for (float x = a.x; x < b.x; x += dash + gap) {
        const float x2 = x + dash < b.x ? x + dash : b.x;
        dl->AddLine(ImVec2(x, a.y), ImVec2(x2, a.y), Rgb(col), t);
        dl->AddLine(ImVec2(x, b.y), ImVec2(x2, b.y), Rgb(col), t);
    }
    for (float y = a.y; y < b.y; y += dash + gap) {
        const float y2 = y + dash < b.y ? y + dash : b.y;
        dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, y2), Rgb(col), t);
        dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, y2), Rgb(col), t);
    }
}

std::string KeyName(int vk) {
    char buf[16];
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
    } else {
        snprintf(buf, sizeof(buf), "0x%02X", vk);
    }
    return buf;
}

bool ActionButton(ImDrawList *dl, const char *id, ImVec2 p, const char *label, const char *key, ButtonKind kind,
                  float &width) {
    const float h = D(48.0f);
    const ImVec2 labelSize = Measure(g_fontSemi, 16.0f, label);
    const float keyWidth = Measure(g_fontLabel, 12.0f, key).x + D(12.0f);
    width = D(20.0f) + labelSize.x + D(12.0f) + keyWidth + D(20.0f);
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 q(p.x + width, p.y + h);
    uint32_t textColor = kText;
    uint32_t keyText = kMuted;
    uint32_t keyBorder = kKeyLine;
    if (kind == ButtonKind::Primary) {
        dl->AddRectFilled(p, q, Rgb(kGreen, hovered ? 1.0f : 0.88f));
        textColor = kOnGreen;
        keyText = kOnGreen;
        keyBorder = kOnGreen;
    } else {
        if (hovered) {
            dl->AddRectFilled(p, q, Rgb(kSelected));
        }
        const uint32_t border = kind == ButtonKind::Danger ? kDanger : kKeyLine;
        dl->AddRect(p, q, Rgb(border), 0.0f, D(1.0f));
        if (kind == ButtonKind::Danger) {
            textColor = kDanger;
        }
    }
    PaintText(dl, g_fontSemi, 16.0f, ImVec2(p.x + D(20.0f), p.y + (h - labelSize.y) * 0.5f), textColor, label);
    KeyHint(dl, ImVec2(p.x + D(20.0f) + labelSize.x + D(12.0f), p.y + D(13.0f)), key, keyText, keyBorder);
    return clicked;
}

bool TextInput(ImVec2 p, float width, float px, const char *hint, char *buffer, size_t size, bool &focusPending) {
    ImGui::SetCursorScreenPos(p);
    ImGui::SetNextItemWidth(width);
    ImGui::PushFont(g_fontSemi, FontBase(px));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgb(kSelected));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgb(kText));
    ImGui::PushStyleColor(ImGuiCol_Border, Rgb(kGreen));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, D(1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(D(10.0f), D(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    if (focusPending) {
        ImGui::SetKeyboardFocusHere();
        focusPending = false;
    }
    const bool enter = ImGui::InputTextWithHint("##title", hint, buffer, size, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    return enter;
}

}
