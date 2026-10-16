#include "XgfFont.h"

#include <EpdFontFamily.h>
#include <FontAlloc.h>
#include <Gfx.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kResidentMax = 6 * 1024 * 1024;
constexpr uint32_t kStreamPrefix = 1024 * 1024;
constexpr uint32_t kInkThreshold = 1;
constexpr uint16_t kMaxCached = 4096;

uint8_t quantize(const uint8_t coverage) {
  // Same bins as the Mac XGF encoder: 255 is solid ink.
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
bool planeKeep(const uint8_t v, const XgfFont::Plane plane) {
  return plane == XgfFont::Plane::Ink && v >= kInkThreshold;
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

void fillRowMask(uint8_t* mask, const uint8_t* src, const uint8_t em, const int rowBytes, const XgfFont::Plane plane) {
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

unsigned long XgfFont::streamRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                  const unsigned long count) {
  auto* self = static_cast<XgfFont*>(ctx);
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

bool XgfFont::load(const char* path, const uint32_t lruMaxBytes, const bool wantRubyMap) {
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

void XgfFont::close() {
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

void XgfFont::releaseFile() {
  if (file.isOpen()) {
    file.close();
  }
}

bool XgfFont::reopenFile() {
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

bool XgfFont::loadBytes() {
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

void XgfFont::freeBytes() {
  fiFontFree(fileBytes);
  fileBytes = nullptr;
  prefixBytes = 0;
}

bool XgfFont::initFaces() {
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Auto;
  options.stemDarkening = true;
  auto openFace = [&](freeink::font::FtFont& face, const uint16_t px) {
    const bool ok =
        streamed ? face.initStream(&XgfFont::streamRead, this, fileSize, px) : face.init(fileBytes, fileSize, px);
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
  facesReady = true;
  return true;
}

void XgfFont::freeFaces() {
  bodyFace.deinit();
  rubyFace.deinit();
  facesReady = false;
}

void XgfFont::releaseMaps() { freeFaces(); }

bool XgfFont::restoreMaps() {
  if (facesReady) {
    return true;
  }
  if (!opened) {
    error = "not loaded";
    return false;
  }
  return initFaces();
}

void XgfFont::freeLru() {
  freeTable(bodyLru);
  freeTable(rubyLru);
}

uint16_t XgfFont::glyphId(const uint32_t cp) const {
  if (!facesReady || cp == 0) {
    return 0xFFFF;
  }
  const auto id = bodyFace.glyphId(cp);
  if (id == 0 || id >= 0xFFFF) {
    return 0xFFFF;
  }
  return static_cast<uint16_t>(id);
}

uint16_t XgfFont::rubySlot(const uint16_t bodyId) const {
  if (!wantRuby || !facesReady || bodyId == 0xFFFF) {
    return 0xFFFF;
  }
  return bodyId;
}

bool XgfFont::allocTable(Lru& t, const uint16_t cap, const uint16_t stride) {
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

void XgfFont::freeTable(Lru& t) {
  heap_caps_free(t.pixels);
  heap_caps_free(t.entries);
  t.pixels = nullptr;
  t.entries = nullptr;
  t.cap = 0;
  t.used = 0;
  t.clock = 1;
  t.stride = 0;
}

bool XgfFont::allocLru() {
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

bool XgfFont::rasterizeSlot(const uint16_t bodyId, const bool ruby, uint8_t* dest) {
  freeink::font::FtFont& face = ruby ? rubyFace : bodyFace;
  const uint8_t em = ruby ? rubyEmPx_ : emPx_;
  const uint16_t stride = slotStride(em);
  memset(dest, 0, stride);
  if (!facesReady || (ruby && !wantRuby)) {
    return false;
  }
  const freeink::font::GlyphBitmap* glyph = face.rasterizeGlyph26_6(bodyId, static_cast<uint32_t>(em) << 6);
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

bool XgfFont::probe(const uint16_t bodyId, const bool ruby, SlotProbe& out) const {
  out = {};
  const Lru& t = ruby ? rubyLru : bodyLru;
  const uint8_t* slot = nullptr;
  for (uint16_t i = 0; i < t.used; ++i) {
    if (t.entries[i].id == bodyId) {
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

XgfFont::CacheStats XgfFont::cacheStats() const {
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

const uint8_t* XgfFont::cacheIn(Lru& t, const uint16_t bodyId, const bool ruby) {
  if (!t.pixels || t.cap == 0) {
    return nullptr;
  }
  uint16_t victim = 0;
  uint16_t oldest = 0xFFFF;
  for (uint16_t i = 0; i < t.used; ++i) {
    if (t.entries[i].id == bodyId) {
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
  if (!rasterizeSlot(bodyId, ruby, dest)) {
    t.entries[slot].id = 0xFFFF;
    if (slot == t.used - 1 && t.used > 0) {
      --t.used;
    }
    return nullptr;
  }
  t.entries[slot].id = bodyId;
  t.entries[slot].recency = ++t.clock;
  return dest;
}

const uint8_t* XgfFont::cacheSlot(const uint16_t bodyId, const bool ruby) {
  if (!bodyLru.pixels && !allocLru()) {
    return nullptr;
  }
  return cacheIn(ruby ? rubyLru : bodyLru, bodyId, ruby);
}

void XgfFont::prewarm(const uint16_t* bodyIds, const uint16_t count, const bool ruby) {
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
      if (table.entries[k].id == order[i]) {
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

bool XgfFont::blit(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const bool ruby, const bool rotate90,
                   const Plane plane) {
  const uint8_t* slot = cacheSlot(bodyId, ruby);
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

bool XgfFont::blitBox(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const int size, const Plane plane) {
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

bool XgfFont::blitUi(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const int size, const bool black) {
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

int XgfFont::drawUtf8(Gfx& gfx, const int fontId, const int x, const int y, const char* text, const bool black,
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
