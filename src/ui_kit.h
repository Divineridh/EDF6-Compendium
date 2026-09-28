#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "imgui.h"

namespace ui {

constexpr uint32_t kBg = 0x0B0E0C;
constexpr uint32_t kLine = 0x242A26;
constexpr uint32_t kSelected = 0x1A201C;
constexpr uint32_t kText = 0xE8ECE9;
constexpr uint32_t kSoft = 0xB7BEBA;
constexpr uint32_t kMuted = 0x8C938F;
constexpr uint32_t kFaint = 0x5A615D;
constexpr uint32_t kGreen = 0x5ED17A;
constexpr uint32_t kOnGreen = 0x0B1A10;
constexpr uint32_t kAmber = 0xF0A73A;
constexpr uint32_t kDiffRow = 0x1B1A13;
constexpr uint32_t kRowLine = 0x1D221F;
constexpr uint32_t kKeyLine = 0x3A413C;
constexpr uint32_t kDanger = 0xFF8C73;
constexpr uint32_t kMaxed = 0xFAC775;

extern ImFont *g_fontLabel;
extern ImFont *g_fontBold;
extern ImFont *g_fontSemi;

void LoadFonts();
void SetScale(float k);
float D(float px);
float FontBase(float px);

ImU32 Rgb(uint32_t hex, float alpha = 1.0f);
ImFont *FontOr(ImFont *f);
ImVec2 Measure(ImFont *f, float px, const char *t, const char *end = nullptr);
void PaintText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const char *t);
float SpacedText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const char *t, float spacing);
std::string Upper(const char *s);
std::string FitText(ImFont *f, float px, const std::string &s, float maxWidth, bool &cut);
void FittedText(ImDrawList *dl, ImFont *f, float px, ImVec2 p, uint32_t col, const std::string &s, float maxWidth);
float KeyHint(ImDrawList *dl, ImVec2 p, const char *key, uint32_t text, uint32_t border);
void DashedRect(ImDrawList *dl, ImVec2 a, ImVec2 b, uint32_t col);
std::string KeyName(int vk);

enum class ButtonKind { Primary, Normal, Danger };
bool ActionButton(ImDrawList *dl, const char *id, ImVec2 p, const char *label, const char *key, ButtonKind kind,
                  float &width);
bool TextInput(ImVec2 p, float width, float px, const char *hint, char *buffer, size_t size, bool &focusPending);

}
