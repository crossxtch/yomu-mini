#include "ReadFace.h"

#include <EpdFontFamily.h>
#include <FontAlloc.h>
#include <Gfx.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kResidentMax = 6 * 1024 * 1024;
constexpr uint32_t kStreamPrefix = 1024 * 1024;
constexpr uint32_t kInkThreshold = 1;
constexpr uint16_t kMaxCached = 4096;

uint8_t quantize(const uint8_t coverage) {
  // 255 is solid ink.
  if (coverage >= 192) return 3;
  if (coverage >= 128) return 2;
  if (coverage >= 64) return 1;
  return 0;
}

uint16_t slotStride(const uint8_t em) {
  const unsigned rowBytes = (static_cast<unsigned>(em) + 3u) / 4u;
  return static_cast<uint16_t>(rowBytes * em);
}

uint8_t pixelAt(const uint8_t* slot, const uint8_t em, const int x, const int y) {
  if (x < 0 || y < 0 || x >= em || y >= em) {
    return 0;
  }
  const unsigned rowBytes = (static_cast<unsigned>(em) + 3u) / 4u;
  const uint8_t b = slot[static_cast<unsigned>(y) * rowBytes + static_cast<unsigned>(x) / 4u];
  const unsigned shift = 6u - (static_cast<unsigned>(x) % 4u) * 2u;
  return static_cast<uint8_t>((b >> shift) & 3u);
}

void putPixel(uint8_t* slot, const uint8_t em, const int x, const int y, const uint8_t v) {
  const unsigned rowBytes = (static_cast<unsigned>(em) + 3u) / 4u;
  uint8_t& b = slot[static_cast<unsigned>(y) * rowBytes + static_cast<unsigned>(x) / 4u];
  const unsigned shift = 6u - (static_cast<unsigned>(x) % 4u) * 2u;
  b = static_cast<uint8_t>((b & ~(3u << shift)) | ((v & 3u) << shift));
}

// 0 paper, 1 light, 2 dark, 3 black. Ink is every non-paper pixel. Gray planes
// stay clear: a marked gray pixel is driven instead of holding the black the
// base frame just painted, and on a reading face those pixels are the stem.
bool planeKeep(const uint8_t v, const ReadFace::Plane plane) {
  return plane == ReadFace::Plane::Ink && v >= kInkThreshold;
}

void paintRowBits(uint8_t* row, const int rowBits, int x, const uint8_t* src, int nBits, const bool black) {
  int s = 0;
  if (x < 0) {
    s = -x;
    nBits -= s;
    x = 0;
  }
  if (nBits <= 0 || x >= rowBits) {
    return;
  }
  if (nBits > rowBits - x) {
    nBits = rowBits - x;
  }
  while (nBits > 0) {
    const int dOff = x & 7;
    const int sOff = s & 7;
    int take = 8 - dOff;
    if (take > 8 - sOff) {
      take = 8 - sOff;
    }
    if (take > nBits) {
      take = nBits;
    }
    uint8_t chunk = static_cast<uint8_t>(src[s >> 3] << sOff);
    chunk = static_cast<uint8_t>(chunk >> dOff);
    const uint8_t m = static_cast<uint8_t>(((1u << take) - 1u) << (8 - dOff - take));
    const uint8_t bits = static_cast<uint8_t>(chunk & m);
    if (black) {
      row[x >> 3] &= static_cast<uint8_t>(~bits);
    } else {
      row[x >> 3] |= bits;
    }
    x += take;
    s += take;
    nBits -= take;
  }
}

void fillRowMask(uint8_t* mask, const uint8_t* src, const uint8_t em, const int rowBytes, const ReadFace::Plane plane) {
  memset(mask, 0, 8);
  int gx = 0;
  for (int b = 0; b < rowBytes && gx < em; ++b) {
    const uint8_t s = src[b];
    for (int n = 0; n < 4 && gx < em; ++n, ++gx) {
      const uint8_t v = static_cast<uint8_t>((s >> (6 - 2 * n)) & 3);
      if (planeKeep(v, plane)) {
        mask[gx >> 3] |= static_cast<uint8_t>(1u << (7 - (gx & 7)));
      }
    }
  }
}

void* cacheAlloc(const size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }
  return p;
}

uint16_t capFor(const uint32_t budget, const uint16_t stride) {
  if (stride == 0 || budget < stride) {
    return 0;
  }
  uint32_t cap = budget / stride;
  if (cap > kMaxCached) {
    cap = kMaxCached;
  }
  return static_cast<uint16_t>(cap);
}

}  // namespace

unsigned long ReadFace::streamRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                  const unsigned long count) {
  auto* self = static_cast<ReadFace*>(ctx);
  if (count == 0) {
    return 0;
  }
  unsigned long done = 0;
  if (offset < self->prefixBytes && self->fileBytes) {
    const unsigned long fromCache = (offset + count <= self->prefixBytes) ? count : self->prefixBytes - offset;
    memcpy(buffer, self->fileBytes + offset, fromCache);
    done = fromCache;
    if (done == count) {
      return count;
    }
  }
  const unsigned long fileOff = offset + done;
  const unsigned long tSd = micros();
  const bool sought = self->file.seekSet(fileOff);
  const int n = sought ? self->file.read(buffer + done, count - done) : 0;
  self->statSdUs += static_cast<uint32_t>(micros() - tSd);
  ++self->statSdReads;
  if (n > 0) {
    self->statSdBytes += static_cast<uint32_t>(n);
    done += static_cast<unsigned long>(n);
  }
  return done;
}

bool ReadFace::load(const char* path, const uint32_t lruMaxBytes, const bool wantRubyMap) {
  close();
  lruLimit = lruMaxBytes ? lruMaxBytes : kLruBytes;
  wantRuby = wantRubyMap;
  emPx_ = kEmPx;
  rubyEmPx_ = kRubyEmPx;
  if (!path || path[0] == '\0') {
    error = "no font path";
    return false;
  }
  snprintf(filepath, sizeof(filepath), "%s", path);
  if (!Storage.openFileForRead("FONT", filepath, file)) {
    error = "font missing";
    LOG_ERR("FONT", "open %s failed", filepath);
    return false;
  }
  fileSize = static_cast<uint32_t>(file.fileSize());
  if (fileSize < 16) {
    error = "font tiny";
    close();
    return false;
  }
  if (!loadBytes() || !initFaces()) {
    close();
    return false;
  }
  opened = true;
  error = "";
  LOG_INF("FONT", "Loaded %s %s %u KB em=%u ruby=%u", filepath, streamed ? "stream" : "resident",
          static_cast<unsigned>(fileSize / 1024), emPx_, rubyEmPx_);
  return true;
}

void ReadFace::close() {
  opened = false;
  freeFaces();
  freeBytes();
  releaseFile();
  freeLru();
  fileSize = 0;
  prefixBytes = 0;
  streamed = false;
  filepath[0] = '\0';
  statHit = statMiss = statFf = statBlocked = 0;
  statSdReads = statSdBytes = statSdUs = 0;
}

void ReadFace::releaseFile() {
  if (file.isOpen()) {
    file.close();
  }
}

bool ReadFace::reopenFile() {
  if (!streamed) {
    return fileBytes != nullptr;
  }
  if (file.isOpen()) {
    return true;
  }
  if (filepath[0] == '\0') {
    error = "font missing";
    return false;
  }
  if (!Storage.openFileForRead("FONT", filepath, file)) {
    error = "font missing";
    return false;
  }
  return true;
}

bool ReadFace::loadBytes() {
  const size_t spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  streamed = fileSize > kResidentMax || spiram < static_cast<size_t>(fileSize) + 256 * 1024;
  if (!streamed) {
    fileBytes = static_cast<uint8_t*>(fiFontMalloc(fileSize));
    if (!fileBytes) {
      streamed = true;
    } else if (!file.seekSet(0) || file.read(fileBytes, fileSize) != static_cast<int>(fileSize)) {
      error = "font read";
      return false;
    } else {
      prefixBytes = fileSize;
      releaseFile();
      return true;
    }
  }
  prefixBytes = fileSize < kStreamPrefix ? fileSize : kStreamPrefix;
  if (spiram < static_cast<size_t>(prefixBytes) + 256 * 1024) {
    prefixBytes = 0;
    return true;
  }
  fileBytes = static_cast<uint8_t*>(fiFontMalloc(prefixBytes));
  if (!fileBytes) {
    prefixBytes = 0;
    return true;
  }
  if (!file.seekSet(0) || file.read(fileBytes, prefixBytes) != static_cast<int>(prefixBytes)) {
    fiFontFree(fileBytes);
    fileBytes = nullptr;
    prefixBytes = 0;
  } else {
    LOG_INF("FONT", "Cached %u KB font prefix", static_cast<unsigned>(prefixBytes / 1024));
  }
  return true;
}

void ReadFace::freeBytes() {
  fiFontFree(fileBytes);
  fileBytes = nullptr;
  prefixBytes = 0;
}

bool ReadFace::initFaces() {
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Auto;
  options.stemDarkening = true;
  auto openFace = [&](freeink::font::FtFont& face, const uint16_t px) {
    const bool ok =
        streamed ? face.initStream(&ReadFace::streamRead, this, fileSize, px) : face.init(fileBytes, fileSize, px);
    if (!ok) {
      return false;
    }
    face.setRenderOptions(options);
    face.setGposByteBudget(0);
    face.setGsubByteBudget(0);
    return true;
  };
  if (!openFace(bodyFace, emPx_)) {
    error = "font parse";
    LOG_ERR("FONT", "FreeType open failed (%s, init %d err %d)", filepath, static_cast<int>(bodyFace.lastInitFailure()),
            bodyFace.lastInitError());
    return false;
  }
  if (wantRuby && !openFace(rubyFace, rubyEmPx_)) {
    error = "ruby face";
    LOG_ERR("FONT", "Ruby face failed err %d", rubyFace.lastInitError());
    freeFaces();
    return false;
  }
  loadVert();
  facesReady = true;
  return true;
}

void ReadFace::freeFaces() {
  clearVert();
  bodyFace.deinit();
  rubyFace.deinit();
  facesReady = false;
}

void ReadFace::releaseMaps() { freeFaces(); }

bool ReadFace::restoreMaps() {
  if (facesReady) {
    return true;
  }
  if (!opened) {
    error = "not loaded";
    return false;
  }
  return initFaces();
}

void ReadFace::freeLru() {
  freeTable(bodyLru);
  freeTable(rubyLru);
}

uint16_t ReadFace::glyphId(const uint32_t cp) const {
  if (!facesReady || cp == 0) {
    return 0xFFFF;
  }
  const auto id = bodyFace.glyphId(cp);
  if (id == 0 || id >= 0xFFFF) {
    return 0xFFFF;
  }
  return static_cast<uint16_t>(id);
}

uint16_t ReadFace::rubySlot(const uint16_t bodyId) const {
  if (!wantRuby || !facesReady || bodyId == 0xFFFF) {
    return 0xFFFF;
  }
  return bodyId;
}

bool ReadFace::allocTable(Lru& t, const uint16_t cap, const uint16_t stride) {
  if (cap == 0 || stride == 0) {
    return false;
  }
  t.pixels = static_cast<uint8_t*>(cacheAlloc(static_cast<size_t>(cap) * stride));
  t.entries = static_cast<LruEntry*>(cacheAlloc(sizeof(LruEntry) * cap));
  if (!t.pixels || !t.entries) {
    freeTable(t);
    return false;
  }
  memset(t.entries, 0, sizeof(LruEntry) * cap);
  for (uint16_t i = 0; i < cap; ++i) {
    t.entries[i].id = 0xFFFF;
  }
  t.cap = cap;
  t.used = 0;
  t.clock = 1;
  t.stride = stride;
  return true;
}

void ReadFace::freeTable(Lru& t) {
  heap_caps_free(t.pixels);
  heap_caps_free(t.entries);
  t.pixels = nullptr;
  t.entries = nullptr;
  t.cap = 0;
  t.used = 0;
  t.clock = 1;
  t.stride = 0;
}

bool ReadFace::allocLru() {
  if (bodyLru.pixels) {
    return true;
  }
  const uint16_t bodyStride = slotStride(emPx_);
  const uint16_t rubyStride = slotStride(rubyEmPx_);
  if (wantRuby) {
    uint32_t rubyBudget = kRubyLruBytes;
    if (rubyBudget > lruLimit) {
      rubyBudget = lruLimit / 4;
    }
    const uint16_t cap = capFor(rubyBudget, rubyStride);
    if (cap == 0 || !allocTable(rubyLru, cap, rubyStride)) {
      LOG_ERR("FONT", "ruby cache oom stride=%u", rubyStride);
    }
  }
  const uint16_t cap = capFor(lruLimit, bodyStride);
  if (cap == 0 || !allocTable(bodyLru, cap, bodyStride)) {
    LOG_ERR("FONT", "body cache oom cap=%u stride=%u", cap, bodyStride);
    error = "lru oom";
    freeLru();
    return false;
  }
  LOG_INF("FONT", "Cache body %u x %u ruby %u x %u", bodyLru.cap, bodyLru.stride, rubyLru.cap, rubyLru.stride);
  return true;
}

bool ReadFace::rasterizeSlot(const uint16_t bodyId, const bool ruby, uint8_t* dest, const bool bold) {
  freeink::font::FtFont& face = ruby ? rubyFace : bodyFace;
  const uint8_t em = ruby ? rubyEmPx_ : emPx_;
  const uint16_t stride = slotStride(em);
  memset(dest, 0, stride);
  if (!facesReady || (ruby && !wantRuby)) {
    return false;
  }
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Auto;
  options.stemDarkening = true;
  options.embolden26_6 = bold ? static_cast<int32_t>(em) * 4 : 0;
  face.setRenderOptions(options);
  const freeink::font::GlyphBitmap* glyph = face.rasterizeGlyph26_6(bodyId, static_cast<uint32_t>(em) << 6);
  if (bold) {
    options.embolden26_6 = 0;
    face.setRenderOptions(options);
  }
  if (!glyph) {
    ++statFf;
    LOG_ERR("FONT", "raster %u %s failed (%d)", bodyId, ruby ? "ruby" : "body", face.lastGlyphError());
    return false;
  }
  if (!glyph->pixels || glyph->width == 0 || glyph->height == 0) {
    return true;
  }
  const int ox = (static_cast<int>(em) - static_cast<int>(glyph->width)) / 2;
  const int oy = (static_cast<int>(em) - static_cast<int>(glyph->height)) / 2;
  for (int sy = 0; sy < static_cast<int>(glyph->height); ++sy) {
    const int dy = oy + sy;
    if (dy < 0 || dy >= em) {
      continue;
    }
    const uint8_t* row = glyph->pixels + static_cast<unsigned>(sy) * glyph->width;
    for (int sx = 0; sx < static_cast<int>(glyph->width); ++sx) {
      const int dx = ox + sx;
      if (dx < 0 || dx >= em) {
        continue;
      }
      const uint8_t v = quantize(row[sx]);
      if (v != 0) {
        putPixel(dest, em, dx, dy, v);
      }
    }
  }
  return true;
}

bool ReadFace::probe(const uint16_t bodyId, const bool ruby, SlotProbe& out) const {
  out = {};
  const Lru& t = ruby ? rubyLru : bodyLru;
  const uint8_t* slot = nullptr;
  for (uint16_t i = 0; i < t.used; ++i) {
    if (t.entries[i].id == bodyId && t.entries[i].style == 0) {
      slot = t.pixels + static_cast<size_t>(i) * t.stride;
      break;
    }
  }
  if (!slot || t.stride == 0) {
    return false;
  }
  out.cached = true;
  uint32_t hash = 2166136261u;
  for (uint16_t i = 0; i < t.stride; ++i) {
    hash ^= slot[i];
    hash *= 16777619u;
  }
  out.hash = hash;
  const uint8_t em = ruby ? rubyEmPx_ : emPx_;
  out.area = static_cast<uint16_t>(em * em);
  uint16_t ink = 0;
  for (int y = 0; y < em; ++y) {
    for (int x = 0; x < em; ++x) {
      if (pixelAt(slot, em, x, y) >= kInkThreshold) {
        ++ink;
      }
    }
  }
  out.ink = ink;
  return true;
}

ReadFace::CacheStats ReadFace::cacheStats() const {
  CacheStats s;
  s.bodyUsed = bodyLru.used;
  s.bodyCap = bodyLru.cap;
  s.rubyUsed = rubyLru.used;
  s.rubyCap = rubyLru.cap;
  s.hits = statHit;
  s.misses = statMiss;
  s.rejected = statFf;
  s.blocked = statBlocked;
  s.sdReads = statSdReads;
  s.sdBytes = statSdBytes;
  s.sdUs = statSdUs;
  return s;
}

const uint8_t* ReadFace::cacheIn(Lru& t, const uint16_t bodyId, const bool ruby, const uint8_t style) {
  if (!t.pixels || t.cap == 0) {
    return nullptr;
  }
  uint16_t victim = 0;
  uint16_t oldest = 0xFFFF;
  for (uint16_t i = 0; i < t.used; ++i) {
    if (t.entries[i].id == bodyId && t.entries[i].style == style) {
      t.entries[i].recency = ++t.clock;
      if (t.clock == 0xFFFF) {
        for (uint16_t j = 0; j < t.used; ++j) {
          t.entries[j].recency = static_cast<uint16_t>(t.entries[j].recency / 2);
        }
        t.clock = 0x8000;
      }
      ++statHit;
      return t.pixels + static_cast<size_t>(i) * t.stride;
    }
    if (t.entries[i].recency <= oldest) {
      oldest = t.entries[i].recency;
      victim = i;
    }
  }
  if (!allowSdReads && streamed) {
    ++statBlocked;
    return nullptr;
  }
  ++statMiss;
  uint16_t slot = victim;
  if (t.used < t.cap) {
    slot = t.used++;
  }
  uint8_t* dest = t.pixels + static_cast<size_t>(slot) * t.stride;
  if (!rasterizeSlot(bodyId, ruby, dest, style != 0)) {
    t.entries[slot].id = 0xFFFF;
    t.entries[slot].style = 0;
    if (slot == t.used - 1 && t.used > 0) {
      --t.used;
    }
    return nullptr;
  }
  t.entries[slot].id = bodyId;
  t.entries[slot].style = style;
  t.entries[slot].recency = ++t.clock;
  return dest;
}

const uint8_t* ReadFace::cacheSlot(const uint16_t bodyId, const bool ruby, const uint8_t style) {
  if (!bodyLru.pixels && !allocLru()) {
    return nullptr;
  }
  return cacheIn(ruby ? rubyLru : bodyLru, bodyId, ruby, style);
}

void ReadFace::prewarm(const uint16_t* bodyIds, const uint16_t count, const bool ruby) {
  if (!bodyIds || count == 0 || !opened) {
    return;
  }
  const unsigned long t0 = millis();
  uint16_t hits = 0;
  uint16_t misses = 0;
  uint16_t order[256];
  uint16_t n = 0;
  for (uint16_t i = 0; i < count && n < 256; ++i) {
    const uint16_t id = bodyIds[i];
    bool dup = false;
    for (uint16_t j = 0; j < n; ++j) {
      if (order[j] == id) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      order[n++] = id;
    }
  }
  for (uint16_t i = 0; i < n; ++i) {
    if (ruby && rubySlot(order[i]) == 0xFFFF) {
      continue;
    }
    const Lru& table = ruby ? rubyLru : bodyLru;
    bool hit = false;
    for (uint16_t k = 0; k < table.used; ++k) {
      if (table.entries[k].id == order[i] && table.entries[k].style == 0) {
        hit = true;
        break;
      }
    }
    if (hit) {
      ++hits;
    } else {
      ++misses;
    }
    (void)cacheSlot(order[i], ruby);
  }
  LOG_INF("FONT", "prewarm %s n=%u hit=%u miss=%u %lums", ruby ? "ruby" : "body", n, hits, misses, millis() - t0);
}

bool ReadFace::blit(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const bool ruby, const bool rotate90,
                   const Plane plane, const uint8_t style) {
  const uint8_t* slot = cacheSlot(bodyId, ruby, style);
  if (!slot) {
    return false;
  }
  const uint8_t em = ruby ? rubyEmPx_ : emPx_;
  if (em == 0) {
    return false;
  }
  uint8_t* fb = gfx.frameBuffer();
  if (!fb) {
    return false;
  }
  const int stride = gfx.stride();
  const int pw = gfx.fbWidth();
  const int ph = gfx.fbHeight();
  const int rowBytes = (static_cast<int>(em) + 3) / 4;

  const bool black = plane == Plane::Ink;
  if (rotate90) {
    uint8_t mask[8];
    for (int gy = 0; gy < em; ++gy) {
      const int phyY = ph - x - em + gy;
      if (phyY < 0 || phyY >= ph) {
        continue;
      }
      fillRowMask(mask, slot + gy * rowBytes, em, rowBytes, plane);
      paintRowBits(fb + phyY * stride, pw, y, mask, em, black);
    }
    return true;
  }

  for (int gy = 0; gy < em; ++gy) {
    const int phyX = y + gy;
    if (phyX < 0 || phyX >= pw) {
      continue;
    }
    const uint8_t clr = static_cast<uint8_t>(~(1u << (7 - (phyX & 7))));
    const int col = phyX >> 3;
    const uint8_t* src = slot + gy * rowBytes;
    int gx = 0;
    for (int b = 0; b < rowBytes && gx < em; ++b) {
      const uint8_t s = src[b];
      for (int n = 0; n < 4 && gx < em; ++n, ++gx) {
        const uint8_t v = static_cast<uint8_t>((s >> (6 - 2 * n)) & 3);
        if (!planeKeep(v, plane)) {
          continue;
        }
        const int phyY = ph - 1 - x - gx;
        if (phyY < 0 || phyY >= ph) {
          continue;
        }
        uint8_t* cell = &fb[phyY * stride + col];
        if (black) {
          *cell &= clr;
        } else {
          *cell |= static_cast<uint8_t>(~clr);
        }
      }
    }
  }
  return true;
}

bool ReadFace::blitBox(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const int size, const Plane plane) {
  if (size <= 0) {
    return false;
  }
  const uint8_t* slot = cacheSlot(bodyId, false);
  if (!slot) {
    return false;
  }
  const uint8_t em = emPx_;
  if (em == 0) {
    return false;
  }
  if (size == static_cast<int>(em)) {
    return blit(gfx, x, y, bodyId, false, false, plane);
  }
  for (int dy = 0; dy < size; ++dy) {
    const int y0 = dy * static_cast<int>(em) / size;
    const int y1 = (dy + 1) * static_cast<int>(em) / size;
    for (int dx = 0; dx < size; ++dx) {
      const int x0 = dx * static_cast<int>(em) / size;
      const int x1 = (dx + 1) * static_cast<int>(em) / size;
      bool ink = false;
      for (int sy = y0; sy < y1 && !ink; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          if (planeKeep(pixelAt(slot, em, sx, sy), plane)) {
            ink = true;
            break;
          }
        }
      }
      if (ink) {
        gfx.drawPixel(x + dx, y + dy, plane == Plane::Ink);
      }
    }
  }
  return true;
}

bool ReadFace::blitUi(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const int size, const bool black) {
  if (size <= 0) {
    return false;
  }
  const uint8_t* slot = cacheSlot(bodyId, false);
  if (!slot) {
    return false;
  }
  const uint8_t em = emPx_;
  if (em == 0) {
    return false;
  }
  if (size == static_cast<int>(em)) {
    for (int gy = 0; gy < em; ++gy) {
      for (int gx = 0; gx < em; ++gx) {
        if (pixelAt(slot, em, gx, gy) >= kInkThreshold) {
          gfx.drawPixel(x + gx, y + gy, black);
        }
      }
    }
    return true;
  }
  for (int dy = 0; dy < size; ++dy) {
    const int y0 = dy * static_cast<int>(em) / size;
    const int y1 = (dy + 1) * static_cast<int>(em) / size;
    for (int dx = 0; dx < size; ++dx) {
      const int x0 = dx * static_cast<int>(em) / size;
      const int x1 = (dx + 1) * static_cast<int>(em) / size;
      bool ink = false;
      for (int sy = y0; sy < y1 && !ink; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          if (pixelAt(slot, em, sx, sy) >= kInkThreshold) {
            ink = true;
            break;
          }
        }
      }
      if (ink) {
        gfx.drawPixel(x + dx, y + dy, black);
      }
    }
  }
  return true;
}

int ReadFace::drawUtf8(Gfx& gfx, const int fontId, const int x, const int y, const char* text, const bool black,
                      const int maxX) {
  if (!text || text[0] == '\0') {
    return 0;
  }
  if (!opened) {
    gfx.drawText(fontId, x, y, text, black);
    return gfx.textWidth(fontId, text);
  }
  const EpdFontFamily* family = gfx.font(fontId);
  const int lineH = family ? family->getData()->advanceY : 16;
  const int asc = family ? family->getData()->ascender : 12;
  int cell = asc > 0 ? asc : lineH;
  if (emPx_ > 0 && emPx_ < cell) {
    cell = emPx_;
  }
  const int cjkY = y + (lineH - cell) / 2;
  int cursorX = x;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp = 0;
  while ((cp = utf8NextCodepoint(&p))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    if (family && family->hasCodepoint(cp)) {
      const int adv = gfx.drawCodepoint(fontId, cursorX, y, cp, black);
      if (adv <= 0) {
        continue;
      }
      if (maxX > 0 && cursorX + adv > maxX) {
        break;
      }
      cursorX += adv;
      continue;
    }
    const uint16_t id = glyphId(cp);
    if (id != 0xFFFF) {
      if (maxX > 0 && cursorX + cell > maxX) {
        break;
      }
      blitUi(gfx, cursorX, cjkY, id, cell, black);
      cursorX += cell;
      continue;
    }
    const int adv = gfx.drawCodepoint(fontId, cursorX, y, cp, black);
    if (adv <= 0) {
      continue;
    }
    if (maxX > 0 && cursorX + adv > maxX) {
      break;
    }
    cursorX += adv;
  }
  return cursorX - x;
}

namespace {

constexpr uint32_t kTagVert = 0x76657274u;
constexpr uint32_t kTagVrt2 = 0x76727432u;
constexpr int kMaxVertPairs = 4096;
constexpr int kMaxVertSubs = 8;
constexpr uint32_t kMaxGsubBytes = 1024u * 1024u;

struct VertSet {
  struct Sub {
    uint16_t* from;
    uint16_t* to;
    uint16_t n;
  };
  Sub subs[kMaxVertSubs];
  int nsubs;
};

struct VertPair {
  uint16_t from;
  uint16_t to;
};

struct GsubView {
  const uint8_t* p;
  size_t n;
  bool u16(const size_t off, uint16_t& o) const {
    if (off + 2 > n) {
      return false;
    }
    o = static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
    return true;
  }
  bool u32(const size_t off, uint32_t& o) const {
    if (off + 4 > n) {
      return false;
    }
    o = (static_cast<uint32_t>(p[off]) << 24) | (static_cast<uint32_t>(p[off + 1]) << 16) |
        (static_cast<uint32_t>(p[off + 2]) << 8) | p[off + 3];
    return true;
  }
};

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

int cmpVertPair(const void* a, const void* b) {
  const uint16_t fa = static_cast<const VertPair*>(a)->from;
  const uint16_t fb = static_cast<const VertPair*>(b)->from;
  if (fa < fb) {
    return -1;
  }
  if (fa > fb) {
    return 1;
  }
  return 0;
}

void freeVert(VertSet* v) {
  if (!v) {
    return;
  }
  for (int i = 0; i < v->nsubs; ++i) {
    heap_caps_free(v->subs[i].from);
    heap_caps_free(v->subs[i].to);
  }
  heap_caps_free(v);
}

bool pushVertPair(VertPair* pairs, int& n, const uint16_t from, const uint16_t to) {
  if (from == 0 || from == to || n >= kMaxVertPairs) {
    return n < kMaxVertPairs;
  }
  pairs[n].from = from;
  pairs[n].to = to;
  ++n;
  return true;
}

template <typename Fn>
bool eachCovered(const GsubView& g, const size_t off, Fn fn) {
  uint16_t fmt = 0;
  uint16_t count = 0;
  if (!g.u16(off, fmt) || !g.u16(off + 2, count)) {
    return false;
  }
  if (fmt == 1) {
    for (uint16_t i = 0; i < count; ++i) {
      uint16_t gid = 0;
      if (!g.u16(off + 4 + static_cast<size_t>(i) * 2, gid)) {
        return false;
      }
      fn(gid, i);
    }
    return true;
  }
  if (fmt == 2) {
    for (uint16_t i = 0; i < count; ++i) {
      const size_t rec = off + 4 + static_cast<size_t>(i) * 6;
      uint16_t start = 0;
      uint16_t end = 0;
      uint16_t index = 0;
      if (!g.u16(rec, start) || !g.u16(rec + 2, end) || !g.u16(rec + 4, index)) {
        return false;
      }
      if (end < start) {
        return false;
      }
      for (uint32_t gid = start; gid <= end; ++gid) {
        fn(static_cast<uint16_t>(gid), static_cast<uint16_t>(index + (gid - start)));
        if (gid == 0xFFFFu) {
          break;
        }
      }
    }
    return true;
  }
  return false;
}

bool addVertSub(VertSet* set, VertPair* pairs, int n) {
  if (n <= 0 || set->nsubs >= kMaxVertSubs) {
    return false;
  }
  qsort(pairs, static_cast<size_t>(n), sizeof(VertPair), cmpVertPair);
  int w = 0;
  for (int i = 0; i < n; ++i) {
    if (w > 0 && pairs[i].from == pairs[w - 1].from) {
      continue;
    }
    pairs[w++] = pairs[i];
  }
  auto* from = static_cast<uint16_t*>(cacheAlloc(static_cast<size_t>(w) * sizeof(uint16_t)));
  auto* to = static_cast<uint16_t*>(cacheAlloc(static_cast<size_t>(w) * sizeof(uint16_t)));
  if (!from || !to) {
    heap_caps_free(from);
    heap_caps_free(to);
    return false;
  }
  for (int i = 0; i < w; ++i) {
    from[i] = pairs[i].from;
    to[i] = pairs[i].to;
  }
  VertSet::Sub& sub = set->subs[set->nsubs];
  sub.from = from;
  sub.to = to;
  sub.n = static_cast<uint16_t>(w);
  ++set->nsubs;
  return true;
}

bool parseSingle(const GsubView& g, const size_t off, VertSet* set, VertPair* pairs) {
  uint16_t fmt = 0;
  uint16_t cov = 0;
  int n = 0;
  if (!g.u16(off, fmt) || !g.u16(off + 2, cov)) {
    return false;
  }
  if (fmt == 1) {
    uint16_t deltaU = 0;
    if (!g.u16(off + 4, deltaU)) {
      return false;
    }
    const int16_t delta = static_cast<int16_t>(deltaU);
    if (!eachCovered(g, off + cov, [&](const uint16_t gid, const uint16_t) {
          const int to = static_cast<int>(gid) + static_cast<int>(delta);
          if (to > 0 && to <= 0xFFFF) {
            pushVertPair(pairs, n, gid, static_cast<uint16_t>(to));
          }
        })) {
      return false;
    }
  } else if (fmt == 2) {
    uint16_t count = 0;
    if (!g.u16(off + 4, count)) {
      return false;
    }
    if (!eachCovered(g, off + cov, [&](const uint16_t gid, const uint16_t index) {
          uint16_t to = 0;
          if (index < count && g.u16(off + 6 + static_cast<size_t>(index) * 2, to)) {
            pushVertPair(pairs, n, gid, to);
          }
        })) {
      return false;
    }
  } else {
    return false;
  }
  if (n > 0) {
    addVertSub(set, pairs, n);
  }
  return true;
}

void parseLookup(const GsubView& g, const size_t lookOff, VertSet* set, VertPair* pairs) {
  uint16_t type = 0;
  uint16_t nsub = 0;
  if (!g.u16(lookOff, type) || !g.u16(lookOff + 4, nsub)) {
    return;
  }
  for (uint16_t i = 0; i < nsub && set->nsubs < kMaxVertSubs; ++i) {
    uint16_t rel = 0;
    if (!g.u16(lookOff + 6 + static_cast<size_t>(i) * 2, rel)) {
      return;
    }
    const size_t sub = lookOff + rel;
    if (type == 1) {
      parseSingle(g, sub, set, pairs);
    } else if (type == 7) {
      uint16_t extFmt = 0;
      uint16_t extType = 0;
      uint32_t extOff = 0;
      if (!g.u16(sub, extFmt) || !g.u16(sub + 2, extType) || !g.u32(sub + 4, extOff)) {
        continue;
      }
      if (extFmt == 1 && extType == 1) {
        parseSingle(g, sub + extOff, set, pairs);
      }
    }
  }
}

VertSet* buildVert(const uint8_t* buf, const size_t len) {
  auto* set = static_cast<VertSet*>(cacheAlloc(sizeof(VertSet)));
  auto* pairs = static_cast<VertPair*>(cacheAlloc(sizeof(VertPair) * kMaxVertPairs));
  if (!set || !pairs) {
    heap_caps_free(set);
    heap_caps_free(pairs);
    return nullptr;
  }
  memset(set, 0, sizeof(VertSet));
  GsubView g{buf, len};
  uint16_t scriptOff = 0;
  uint16_t featOff = 0;
  uint16_t lookOff = 0;
  uint16_t featCount = 0;
  uint16_t lookCount = 0;
  if (!g.u16(4, scriptOff) || !g.u16(6, featOff) || !g.u16(8, lookOff) || !g.u16(featOff, featCount) ||
      !g.u16(lookOff, lookCount)) {
    heap_caps_free(pairs);
    freeVert(set);
    return nullptr;
  }
  (void)scriptOff;
  int featIndex = -1;
  for (uint16_t fi = 0; fi < featCount; ++fi) {
    const size_t rec = static_cast<size_t>(featOff) + 2 + static_cast<size_t>(fi) * 6;
    uint32_t tag = 0;
    if (!g.u32(rec, tag)) {
      break;
    }
    if (tag == kTagVert) {
      featIndex = static_cast<int>(fi);
      break;
    }
    if (tag == kTagVrt2 && featIndex < 0) {
      featIndex = static_cast<int>(fi);
    }
  }
  if (featIndex >= 0) {
    const size_t rec = static_cast<size_t>(featOff) + 2 + static_cast<size_t>(featIndex) * 6;
    uint16_t frel = 0;
    uint16_t nlook = 0;
    if (g.u16(rec + 4, frel) && g.u16(static_cast<size_t>(featOff) + frel + 2, nlook)) {
      const size_t feature = static_cast<size_t>(featOff) + frel;
      for (uint16_t li = 0; li < nlook && set->nsubs < kMaxVertSubs; ++li) {
        uint16_t which = 0;
        uint16_t lrel = 0;
        if (!g.u16(feature + 4 + static_cast<size_t>(li) * 2, which)) {
          break;
        }
        if (which >= lookCount || !g.u16(static_cast<size_t>(lookOff) + 2 + static_cast<size_t>(which) * 2, lrel)) {
          continue;
        }
        parseLookup(g, static_cast<size_t>(lookOff) + lrel, set, pairs);
      }
    }
  }
  heap_caps_free(pairs);
  if (set->nsubs == 0) {
    freeVert(set);
    return nullptr;
  }
  return set;
}

}  // namespace

void ReadFace::clearVert() {
  freeVert(static_cast<VertSet*>(vert));
  vert = nullptr;
}

bool ReadFace::readFont(const uint32_t off, uint8_t* dst, const uint32_t n) {
  if (n == 0) {
    return true;
  }
  if (!dst) {
    return false;
  }
  if (fileBytes) {
    const uint32_t have = streamed ? prefixBytes : fileSize;
    if (off <= have && n <= have - off) {
      memcpy(dst, fileBytes + off, n);
      return true;
    }
  }
  if (!file.isOpen()) {
    return false;
  }
  return streamRead(this, off, dst, n) == n;
}

void ReadFace::loadVert() {
  clearVert();
  uint8_t hdr[12]{};
  if (!readFont(0, hdr, sizeof(hdr))) {
    return;
  }
  uint32_t sfnt = 0;
  if (hdr[0] == 't' && hdr[1] == 't' && hdr[2] == 'c' && hdr[3] == 'f') {
    if (be32(hdr + 8) < 1) {
      return;
    }
    uint8_t ent[4]{};
    if (!readFont(12, ent, sizeof(ent))) {
      return;
    }
    sfnt = be32(ent);
  }
  uint8_t sh[12]{};
  if (!readFont(sfnt, sh, sizeof(sh))) {
    return;
  }
  uint16_t nTables = be16(sh + 4);
  if (nTables > 64) {
    nTables = 64;
  }
  uint32_t off = 0;
  uint32_t len = 0;
  bool found = false;
  for (uint16_t i = 0; i < nTables; ++i) {
    uint8_t rec[16]{};
    if (!readFont(sfnt + 12 + static_cast<uint32_t>(i) * 16, rec, sizeof(rec))) {
      return;
    }
    if (rec[0] == 'G' && rec[1] == 'S' && rec[2] == 'U' && rec[3] == 'B') {
      off = be32(rec + 8);
      len = be32(rec + 12);
      found = true;
      break;
    }
  }
  if (!found || len < 10 || len > kMaxGsubBytes) {
    return;
  }
  const uint32_t have = fileBytes ? (streamed ? prefixBytes : fileSize) : 0;
  const bool covered = fileBytes && off <= have && len <= have - off;
  uint8_t* owned = nullptr;
  const uint8_t* view = nullptr;
  if (covered) {
    view = fileBytes + off;
  } else {
    owned = static_cast<uint8_t*>(cacheAlloc(len));
    if (!owned || !readFont(off, owned, len)) {
      heap_caps_free(owned);
      return;
    }
    view = owned;
  }
  vert = buildVert(view, len);
  heap_caps_free(owned);
  const auto* set = static_cast<const VertSet*>(vert);
  if (set && set->nsubs > 0) {
    LOG_INF("FONT", "vert subs %d", set->nsubs);
  }
}

uint16_t ReadFace::vertOf(const uint16_t gid) const {
  const auto* v = static_cast<const VertSet*>(vert);
  if (!v || gid == 0) {
    return gid;
  }
  uint16_t g = gid;
  for (int s = 0; s < v->nsubs; ++s) {
    const VertSet::Sub& sub = v->subs[s];
    int lo = 0;
    int hi = sub.n;
    while (lo < hi) {
      const int mid = lo + (hi - lo) / 2;
      if (sub.from[mid] < g) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo < sub.n && sub.from[lo] == g) {
      g = sub.to[lo];
    }
  }
  return g;
}

int16_t ReadFace::advancePx(const uint32_t cp, const int px) {
  if (!facesReady || px < 1) {
    return 0;
  }
  int size = px;
  if (size > 255) {
    size = 255;
  }
  return bodyFace.advance(cp, static_cast<uint16_t>(size), 0);
}

bool ReadFace::blitCell(Gfx& gfx, const int x, const int y, const int cellW, const int cellH, const uint16_t bodyId,
                       int px, const bool vertical, const bool bold, const bool rotate90, const bool yakumono,
                       const Plane plane) {
  if (plane != Plane::Ink) {
    return true;
  }
  if (!facesReady || cellW < 1 || cellH < 1 || bodyId == 0 || bodyId == 0xFFFF) {
    return false;
  }
  if (px < 1) {
    px = 1;
  }
  if (px > 128) {
    px = 128;
  }
  uint16_t gid = bodyId;
  bool slide = false;
  if (vertical) {
    const uint16_t v = vertOf(gid);
    if (v != 0 && v != gid) {
      gid = v;
      slide = true;
    }
  }
  // A full em keeps the centered cache, including a separate bold entry.
  if (!slide && !rotate90 && px == static_cast<int>(emPx_) && cellW == px && cellH == px) {
    return blit(gfx, x, y, gid, false, false, plane, bold ? 1 : 0);
  }

  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Auto;
  options.stemDarkening = true;
  options.embolden26_6 = bold ? px * 4 : 0;
  bodyFace.setRenderOptions(options);
  freeink::font::FtFont::LineMetrics lm{};
  const bool haveLm = bodyFace.lineMetrics26_6(static_cast<uint32_t>(px) << 6, lm);
  const freeink::font::GlyphBitmap* glyph = bodyFace.rasterizeGlyph26_6(gid, static_cast<uint32_t>(px) << 6);
  options.embolden26_6 = 0;
  bodyFace.setRenderOptions(options);
  if (!glyph || !glyph->pixels || glyph->width == 0 || glyph->height == 0) {
    return glyph != nullptr;
  }

  const uint8_t* bits = glyph->pixels;
  int bw = glyph->width;
  int bh = glyph->height;
  uint8_t* turned = nullptr;
  if (rotate90 && !slide && bw > 0 && bh > 0 && bw <= 128 && bh <= 128) {
    const int rw = bh;
    const int rh = bw;
    turned = static_cast<uint8_t*>(cacheAlloc(static_cast<size_t>(rw) * static_cast<size_t>(rh)));
    if (turned) {
      memset(turned, 0, static_cast<size_t>(rw) * static_cast<size_t>(rh));
      for (int gy = 0; gy < bh; ++gy) {
        for (int gx = 0; gx < bw; ++gx) {
          const int nx = bh - 1 - gy;
          const int ny = gx;
          turned[ny * rw + nx] = bits[gy * bw + gx];
        }
      }
      bits = turned;
      bw = rw;
      bh = rh;
    }
  }

  auto stamp = [&](const int ox, const int oy) {
    for (int gy = 0; gy < bh; ++gy) {
      const int dy = oy + gy;
      if (dy < 0 || dy >= cellH) {
        continue;
      }
      const uint8_t* row = bits + static_cast<size_t>(gy) * static_cast<size_t>(bw);
      for (int gx = 0; gx < bw; ++gx) {
        const int dx = ox + gx;
        if (dx < 0 || dx >= cellW) {
          continue;
        }
        if (quantize(row[gx]) == 0) {
          continue;
        }
        gfx.drawPixel(x + dx, y + dy, true);
      }
    }
  };

  const bool center = !slide && (yakumono || rotate90 || turned || (cellW == px && cellH == px));
  if (center || !haveLm) {
    stamp((cellW - bw) / 2, (cellH - bh) / 2);
    heap_caps_free(turned);
    return true;
  }

  int ascent = static_cast<int>(lm.ascender26_6 >> 6);
  int descent = static_cast<int>(-(lm.descender26_6 >> 6));
  if (ascent < 0) {
    ascent = 0;
  }
  if (descent < 0) {
    descent = 0;
  }
  if (ascent + descent <= 0) {
    stamp((cellW - bw) / 2, (cellH - bh) / 2);
    heap_caps_free(turned);
    return true;
  }
  const int boxW = slide ? px : cellW;
  const int boxH = slide ? px : cellH;
  const int pad = (boxH - (ascent + descent)) / 2;
  int baseline = boxH - descent - pad;
  if (slide && cellH < px) {
    const int top = -glyph->yoff;
    const int mid = top - static_cast<int>(glyph->height) / 2;
    const int centerY = (ascent - descent) / 2;
    if (mid < centerY) {
      baseline -= px - cellH;
    }
  }
  int adv = glyph->advance > 0 ? glyph->advance : px;
  const int penX = (boxW - adv) / 2;
  stamp(penX + glyph->xoff, baseline + glyph->yoff);
  heap_caps_free(turned);
  return true;
}
