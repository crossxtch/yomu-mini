#pragma once

#include <BookFace.h>

#include <cstddef>

// A reading face is a TrueType file under /.yomu/fonts/. There is no built-in
// face. An empty selection, or a missing file, leaves the reader without a font.
namespace ReadingFont {

constexpr const char* kDir = "/.yomu/fonts";
constexpr size_t kMaxFileName = 79;

void migrate();

bool isFontFilename(const char* name);
bool copyFilename(char* out, size_t outSize, const char* name);
void makePath(char* out, size_t outSize, const char* filename);

// Path BookFace::load understands. False when nothing usable is selected.
bool activePath(char* out, size_t outSize);
bool setActive(const char* filename);
bool loadUi(BookFace& font);

}  // namespace ReadingFont
