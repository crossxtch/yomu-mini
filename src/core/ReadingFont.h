#pragma once

#include <BookFace.h>

#include <cstddef>

// The default reading faces are built in (prebaked). A file under
// /.yomu/fonts/ is either another prebaked face (.xgf2) or a TrueType face
// rasterized on device (.ttf .otf .ttc).
namespace ReadingFont {

constexpr const char* kDir = "/.yomu/fonts";
constexpr const char* kMinchoId = "*mincho";
constexpr const char* kGothicId = "*gothic";
constexpr size_t kMaxFileName = 79;

void migrate();

bool isBuiltinId(const char* name);
bool isFontFilename(const char* name);
bool copyFilename(char* out, size_t outSize, const char* name);
void makePath(char* out, size_t outSize, const char* filename);

// Path BookFace::load understands. Built-ins use the builtin: sentinels.
// A missing selected file falls back to the built-in mincho.
bool activePath(char* out, size_t outSize);
bool setActive(const char* filename);
bool loadUi(BookFace& font);

}  // namespace ReadingFont
