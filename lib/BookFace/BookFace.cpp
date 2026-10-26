#include "BookFace.h"

#include <Gfx.h>
#include <Memory.h>

#include <cstdio>

bool BookFace::load(const char* path, const uint32_t lruMaxBytes, const bool wantRuby) {
  close();
  if (!path || path[0] == '\0') {
    error = "font missing";
    return false;
  }
  snprintf(key, sizeof(key), "%s", path);
  raster = makeUniqueNoThrow<ReadFace>();
  if (!raster) {
    error = "out of memory";
    return false;
  }
  if (!raster->load(path, lruMaxBytes, wantRuby)) {
    error = raster->lastError();
    raster.reset();
    return false;
  }
  return true;
}

void BookFace::close() {
  raster.reset();
  key[0] = '\0';
  error = "not loaded";
}

void BookFace::releaseMaps() {
  if (raster) {
    raster->releaseMaps();
  }
}

bool BookFace::restoreMaps() {
  return raster && raster->restoreMaps();
}

void BookFace::releaseFile() {
  if (raster) {
    raster->releaseFile();
  }
}

bool BookFace::reopenFile() {
  return raster && raster->reopenFile();
}

uint8_t BookFace::emPx() const {
  return raster ? raster->emPx() : 0;
}

uint8_t BookFace::rubyEmPx() const {
  return raster ? raster->rubyEmPx() : 0;
}

uint16_t BookFace::glyphId(const uint32_t cp) const {
  return raster ? raster->glyphId(cp) : 0xFFFF;
}

void BookFace::prewarm(const uint16_t* bodyIds, const uint16_t count, const bool ruby) {
  if (raster) {
    raster->prewarm(bodyIds, count, ruby);
  }
}

void BookFace::setSdReads(const bool allow) {
  if (raster) {
    raster->setSdReads(allow);
  }
}

bool BookFace::probe(const uint16_t bodyId, const bool ruby, SlotProbe& out) const {
  if (raster) {
    return raster->probe(bodyId, ruby, out);
  }
  out = {};
  return false;
}

BookFace::CacheStats BookFace::cacheStats() const {
  return raster ? raster->cacheStats() : CacheStats{};
}

int16_t BookFace::advancePx(const uint32_t cp, const int px) {
  return raster ? raster->advancePx(cp, px) : 0;
}

bool BookFace::blit(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const bool ruby, const bool rotate90,
                    const Plane plane) {
  return raster && raster->blit(gfx, x, y, bodyId, ruby, rotate90, plane);
}

bool BookFace::blitCell(Gfx& gfx, const int x, const int y, const int cellW, const int cellH, const uint16_t bodyId,
                        const int px, const bool vertical, const bool bold, const bool rotate90, const bool yakumono,
                        const Plane plane) {
  return raster && raster->blitCell(gfx, x, y, cellW, cellH, bodyId, px, vertical, bold, rotate90, yakumono, plane);
}

bool BookFace::blitBox(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const int size, const Plane plane) {
  return raster && raster->blitBox(gfx, x, y, bodyId, size, plane);
}

int BookFace::drawUtf8(Gfx& gfx, const int fontId, const int x, const int y, const char* text, const bool black,
                       const int maxX) {
  if (raster) {
    return raster->drawUtf8(gfx, fontId, x, y, text, black, maxX);
  }
  gfx.drawText(fontId, x, y, text ? text : "", black);
  return text ? gfx.textWidth(fontId, text) : 0;
}

void BookFace::metricsId(char* out, const size_t outSize) const {
  if (!out || outSize < 9) {
    return;
  }
  uint32_t h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(key); *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  snprintf(out, outSize, "%08lx", static_cast<unsigned long>(h));
}
