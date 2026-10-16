#pragma once

#include <XgfFont.h>

#include <cstddef>

// Installed TrueType faces (.ttf .otf .ttc) under /.crossjp/fonts/. One is
// selected in Settings and rasterized on device for reading and UI CJK.
namespace ReadingFont {

constexpr const char* kDir = "/.crossjp/fonts";
constexpr size_t kMaxFileName = 79;

void migrate();

bool isFontFilename(const char* name);
bool copyFilename(char* out, size_t outSize, const char* name);
void makePath(char* out, size_t outSize, const char* filename);

// Absolute path of the selected face, or the first installed one. false if none.
bool activePath(char* out, size_t outSize);
bool setActive(const char* filename);
bool loadUi(XgfFont& font);

}  // namespace ReadingFont
