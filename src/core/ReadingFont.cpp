#include "core/ReadingFont.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "core/Settings.h"

namespace fontfile {

bool endsWithI(const char* name, const char* ext) {
  const size_t n = strlen(name);
  const size_t e = strlen(ext);
  if (n < e) {
    return false;
  }
  for (size_t i = 0; i < e; ++i) {
    char a = name[n - e + i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (a != ext[i]) {
      return false;
    }
  }
  return true;
}

bool fontExt(const char* name) {
  return endsWithI(name, ".ttf") || endsWithI(name, ".otf") || endsWithI(name, ".ttc");
}

bool illegalFatChar(const char c) {
  return c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
}

bool stripDuplicateExt(char* name) {
  const size_t n = strlen(name);
  for (const char* ext : {".ttf", ".otf", ".ttc"}) {
    const size_t e = strlen(ext);
    if (n > e * 2 && endsWithI(name, ext) && endsWithI(name + n - e * 2, ext)) {
      name[n - e] = '\0';
      return true;
    }
  }
  return false;
}

}  // namespace fontfile

void ReadingFont::migrate() {
  Storage.ensureDirectoryExists(Settings::kDir);
  Storage.ensureDirectoryExists(kDir);
  // A stored *mincho, *gothic, or .xgf2 name is no longer a face.
  if (settings.fontFile[0] != '\0' && !isFontFilename(settings.fontFile)) {
    LOG_INF("FONT", "Dropping %s", settings.fontFile);
    settings.fontFile[0] = '\0';
    settings.save();
  }
}

bool ReadingFont::isFontFilename(const char* name) {
  if (!name || name[0] == '\0' || name[0] == '.') {
    return false;
  }
  const size_t n = strlen(name);
  if (n > kMaxFileName || !fontfile::fontExt(name)) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (fontfile::illegalFatChar(name[i])) {
      return false;
    }
  }
  return true;
}

bool ReadingFont::copyFilename(char* out, const size_t outSize, const char* name) {
  if (!out || outSize == 0) {
    return false;
  }
  out[0] = '\0';
  if (!name) {
    return false;
  }
  const char* slash = strrchr(name, '/');
  const char* base = slash ? slash + 1 : name;
  snprintf(out, outSize, "%s", base);
  // A save panel that appends the extension again yields "Face.ttf.ttf".
  fontfile::stripDuplicateExt(out);
  if (!isFontFilename(out)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

void ReadingFont::makePath(char* out, const size_t outSize, const char* filename) {
  snprintf(out, outSize, "%s/%s", kDir, filename ? filename : "");
}

bool ReadingFont::activePath(char* out, const size_t outSize) {
  if (!out || outSize == 0) {
    return false;
  }
  out[0] = '\0';
  const char* id = settings.fontFile;
  if (!isFontFilename(id)) {
    return false;
  }
  makePath(out, outSize, id);
  if (Storage.exists(out)) {
    return true;
  }
  LOG_INF("FONT", "Missing %s", id);
  out[0] = '\0';
  return false;
}

bool ReadingFont::setActive(const char* filename) {
  char name[kMaxFileName + 1];
  if (!copyFilename(name, sizeof(name), filename)) {
    return false;
  }
  char path[192];
  makePath(path, sizeof(path), name);
  if (!Storage.exists(path)) {
    LOG_ERR("FONT", "Missing %s", path);
    return false;
  }
  snprintf(settings.fontFile, sizeof(settings.fontFile), "%s", name);
  settings.save();
  LOG_INF("FONT", "Active %s", name);
  return true;
}

bool ReadingFont::loadUi(BookFace& font) {
  char path[192];
  if (!activePath(path, sizeof(path))) {
    return false;
  }
  return font.load(path, ReadFace::kUiLruBytes, false);
}
