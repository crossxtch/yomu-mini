#include "XgfFont.h"

#include <EpdFontFamily.h>
#include <Gfx.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <cstring>

// Raw DEFLATE inflate. Defined in lib/Book/puff.c. The YGZF payload is a zlib
// wrapper (RFC 1950); this reader does not link zlib.
extern "C" int puff(unsigned char* dest, unsigned long* destlen, const unsigned char* source,
                    unsigned long* sourcelen);

namespace {

constexpr uint32_t kPackMagic = 0x465A4759;  // YGZF

uint8_t pixelAt(const uint8_t* slot, const uint8_t em, const int x, const int y) {
  if (!slot || x < 0 || y < 0 || x >= em || y >= em) {
    return 0;
  }
  const unsigned rowBytes = (static_cast<unsigned>(em) + 3u) / 4u;
  const uint8_t b = slot[static_cast<unsigned>(y) * rowBytes + static_cast<unsigned>(x) / 4u];
  const unsigned shift = 6u - (static_cast<unsigned>(x) % 4u) * 2u;
  return static_cast<uint8_t>((b >> shift) & 3u);
}

// Ink is every non-paper pixel. Gray planes stay empty: a marked gray pixel
// on this panel is driven as a block, not as a stem.
bool planeKeep(const uint8_t v, const XgfFont::Plane plane) {
  return plane == XgfFont::Plane::Ink && v >= xgf::kInkThreshold;
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

bool rangeInside(const uint32_t off, const uint32_t len, const uint32_t size) {
  return len <= size && off <= size - len;
}

}  // namespace

bool XgfFont::adopt(uint8_t* bytes, const uint32_t n) {
  close();
  if (!bytes || n < xgf::kHeaderSize) {
    heap_caps_free(bytes);
    error = "invalid format";
    return false;
  }
  image = bytes;
  imageSize = n;
  if (!parse()) {
    const char* why = error;
    close();
    error = why;
    return false;
  }
  opened = true;
  error = "";
  LOG_INF("XGF", "resident em=%u glyphs=%u bytes=%lu", header.emPx, header.bodyCount,
          static_cast<unsigned long>(imageSize));
  return true;
}

bool XgfFont::parse() {
  memcpy(&header, image, sizeof(header));
  if (header.magic != xgf::kMagic || header.version != xgf::kVersion) {
    error = "invalid format";
    return false;
  }
  if (header.bpp != 2 || header.emPx < 4 || header.emPx > 64) {
    error = "invalid format";
    return false;
  }
  if (header.bodyStride != xgf::slotStride(header.emPx)) {
    error = "invalid format";
    return false;
  }
  if (header.rubyEmPx > 0 && header.rubyStride != xgf::slotStride(header.rubyEmPx)) {
    error = "invalid format";
    return false;
  }
  if (header.intervalCount == 0 || header.intervalCount > xgf::kMaxIntervals || header.bodyCount == 0 ||
      header.bodyCount > xgf::kMaxGlyphs) {
    error = "invalid format";
    return false;
  }
  const uint32_t ivBytes = static_cast<uint32_t>(header.intervalCount) * sizeof(xgf::Interval);
  const uint32_t bodyBytes = static_cast<uint32_t>(header.bodyCount) * header.bodyStride;
  if (!rangeInside(header.intervalsOff, ivBytes, imageSize) ||
      !rangeInside(header.bodyBitsOff, bodyBytes, imageSize)) {
    error = "invalid format";
    return false;
  }
  if (header.rubyCount > 0) {
    const uint32_t rubyBytes = static_cast<uint32_t>(header.rubyCount) * header.rubyStride;
    const uint32_t mapBytes = static_cast<uint32_t>(header.rubyCount) * sizeof(uint16_t);
    if (!rangeInside(header.rubyMapOff, mapBytes, imageSize) ||
        (header.rubyBitsOff != 0 && !rangeInside(header.rubyBitsOff, rubyBytes, imageSize))) {
      error = "invalid format";
      return false;
    }
  }

  intervals = makeUniqueNoThrow<xgf::Interval[]>(header.intervalCount);
  if (!intervals) {
    error = "out of memory";
    return false;
  }
  memcpy(intervals.get(), image + header.intervalsOff, ivBytes);
  uint32_t covered = 0;
  for (uint16_t i = 0; i < header.intervalCount; ++i) {
    const xgf::Interval& iv = intervals[i];
    if (iv.first > iv.last || (i > 0 && iv.first <= intervals[i - 1].last)) {
      error = "invalid format";
      return false;
    }
    const uint32_t span = static_cast<uint32_t>(iv.last - iv.first) + 1u;
    if (static_cast<uint32_t>(iv.glyphId) + span > header.bodyCount) {
      error = "invalid format";
      return false;
    }
    covered += span;
  }
  if (covered != header.bodyCount) {
    error = "invalid format";
    return false;
  }

  if (wantRuby && header.rubyCount > 0) {
    rubyMap = makeUniqueNoThrow<uint16_t[]>(header.rubyCount);
    if (!rubyMap) {
      error = "out of memory";
      return false;
    }
    memcpy(rubyMap.get(), image + header.rubyMapOff, static_cast<size_t>(header.rubyCount) * sizeof(uint16_t));
  }
  buildHi();
  return true;
}

void XgfFont::buildHi() {
  memset(hi, 0, sizeof(hi));
  for (uint16_t i = 0; i < header.intervalCount; ++i) {
    const uint16_t b0 = static_cast<uint16_t>(intervals[i].first >> 8);
    const uint16_t b1 = static_cast<uint16_t>(intervals[i].last >> 8);
    for (uint16_t b = b0; b <= b1; ++b) {
      if (hi[b].count == 0) {
        hi[b].begin = i;
      }
      hi[b].count = static_cast<uint16_t>(i - hi[b].begin + 1);
    }
  }
}

bool XgfFont::loadPacked(const uint8_t* packed, const size_t packedBytes, const bool wantRubyMap) {
  close();
  wantRuby = wantRubyMap;
  if (!packed || packedBytes < 8) {
    error = "font missing";
    return false;
  }
  uint32_t magic = 0;
  uint32_t rawSize = 0;
  memcpy(&magic, packed, 4);
  memcpy(&rawSize, packed + 4, 4);
  if (magic != kPackMagic || rawSize < xgf::kHeaderSize || rawSize > kMaxImage || packedBytes < 18) {
    error = "invalid format";
    return false;
  }
  // CMF/FLG, then raw DEFLATE, then a 4-byte Adler checksum. No preset dictionary.
  const unsigned cmf = packed[8];
  const unsigned flg = packed[9];
  if ((cmf & 0x0fu) != 8u || (cmf >> 4) > 7u || ((cmf << 8) | flg) % 31u != 0u || (flg & 0x20u) != 0u) {
    error = "invalid format";
    return false;
  }
  // One PSRAM block for the uncompressed face. Freed in close().
  auto* raw = static_cast<uint8_t*>(heap_caps_malloc(rawSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) {
    LOG_ERR("XGF", "inflate oom %lu", static_cast<unsigned long>(rawSize));
    error = "out of memory";
    return false;
  }
  unsigned long dest = rawSize;
  unsigned long srcLen = static_cast<unsigned long>(packedBytes - 10);
  const int z = puff(raw, &dest, packed + 10, &srcLen);
  if (z != 0 || dest != rawSize) {
    heap_caps_free(raw);
    LOG_ERR("XGF", "inflate failed %d", z);
    error = "invalid format";
    return false;
  }
  return adopt(raw, rawSize);
}

bool XgfFont::load(const char* path, const bool wantRubyMap) {
  close();
  wantRuby = wantRubyMap;
  if (!path || path[0] == '\0') {
    error = "font missing";
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("XGF", path, file)) {
    error = "file not found";
    return false;
  }
  const size_t n = file.fileSize();
  if (n < xgf::kHeaderSize || n > kMaxImage) {
    error = n > kMaxImage ? "file too large" : "file too small";
    return false;
  }
  // The whole face is read once. Page turns then stay off the card. Freed in close().
  auto* raw = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) {
    LOG_ERR("XGF", "read oom %u", static_cast<unsigned>(n));
    error = "out of memory";
    return false;
  }
  size_t got = 0;
  while (got < n) {
    const int r = file.read(raw + got, n - got);
    if (r <= 0) {
      break;
    }
    got += static_cast<size_t>(r);
  }
  if (got != n) {
    heap_caps_free(raw);
    error = "could not read file";
    return false;
  }
  uint32_t magic = 0;
  memcpy(&magic, raw, 4);
  if (magic == kPackMagic) {
    const bool ok = loadPacked(raw, n, wantRubyMap);
    heap_caps_free(raw);
    return ok;
  }
  return adopt(raw, static_cast<uint32_t>(n));
}

void XgfFont::close() {
  heap_caps_free(image);
  image = nullptr;
  imageSize = 0;
  header = {};
  intervals.reset();
  rubyMap.reset();
  memset(hi, 0, sizeof(hi));
  opened = false;
  statHit = 0;
  error = "not loaded";
}

uint16_t XgfFont::glyphId(const uint32_t cp) const {
  if (!intervals || cp > 0xFFFF) {
    return 0xFFFF;
  }
  const uint16_t b = static_cast<uint16_t>(cp >> 8);
  const uint16_t begin = hi[b].begin;
  const uint16_t n = hi[b].count;
  if (n == 0) {
    return 0xFFFF;
  }
  uint16_t lo = begin;
  uint16_t hiIx = static_cast<uint16_t>(begin + n);
  while (lo < hiIx) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hiIx - lo) / 2);
    const xgf::Interval& iv = intervals[mid];
    if (cp < iv.first) {
      hiIx = mid;
    } else if (cp > iv.last) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      return static_cast<uint16_t>(iv.glyphId + (cp - iv.first));
    }
  }
  return 0xFFFF;
}

uint16_t XgfFont::rubySlot(const uint16_t bodyId) const {
  if (!rubyMap || header.rubyCount == 0) {
    return 0xFFFF;
  }
  uint16_t lo = 0;
  uint16_t hiIx = header.rubyCount;
  while (lo < hiIx) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hiIx - lo) / 2);
    if (rubyMap[mid] < bodyId) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      hiIx = mid;
    }
  }
  if (lo < header.rubyCount && rubyMap[lo] == bodyId) {
    return lo;
  }
  return 0xFFFF;
}

const uint8_t* XgfFont::slotBytes(const uint16_t bodyId, const bool ruby) const {
  if (!image) {
    return nullptr;
  }
  uint32_t off = 0;
  uint16_t stride = 0;
  if (ruby) {
    const uint16_t slot = rubySlot(bodyId);
    if (slot == 0xFFFF || header.rubyStride == 0) {
      return nullptr;
    }
    stride = header.rubyStride;
    off = header.rubyBitsOff + static_cast<uint32_t>(slot) * stride;
  } else {
    if (bodyId >= header.bodyCount) {
      return nullptr;
    }
    stride = header.bodyStride;
    off = header.bodyBitsOff + static_cast<uint32_t>(bodyId) * stride;
  }
  if (!rangeInside(off, stride, imageSize)) {
    return nullptr;
  }
  ++statHit;
  return image + off;
}

bool XgfFont::probe(const uint16_t bodyId, const bool ruby, SlotProbe& out) const {
  out = {};
  const uint8_t* slot = slotBytes(bodyId, ruby);
  if (!slot) {
    return false;
  }
  out.cached = true;
  const uint16_t stride = ruby ? header.rubyStride : header.bodyStride;
  uint32_t hash = 2166136261u;
  for (uint16_t i = 0; i < stride; ++i) {
    hash ^= slot[i];
    hash *= 16777619u;
  }
  out.hash = hash;
  const uint8_t em = ruby ? header.rubyEmPx : header.emPx;
  out.area = static_cast<uint16_t>(em * em);
  uint16_t ink = 0;
  for (int y = 0; y < em; ++y) {
    for (int x = 0; x < em; ++x) {
      if (pixelAt(slot, em, x, y) >= xgf::kInkThreshold) {
        ++ink;
      }
    }
  }
  out.ink = ink;
  return true;
}

XgfFont::CacheStats XgfFont::cacheStats() const {
  CacheStats s;
  s.bodyUsed = header.bodyCount;
  s.bodyCap = header.bodyCount;
  s.rubyUsed = header.rubyCount;
  s.rubyCap = header.rubyCount;
  s.hits = statHit;
  return s;
}

void XgfFont::prewarm(const uint16_t* bodyIds, const uint16_t count, const bool ruby) {
  (void)bodyIds;
  (void)count;
  (void)ruby;
}

int16_t XgfFont::advancePx(const uint32_t cp, const int px) {
  if (!opened || px < 1 || glyphId(cp) == 0xFFFF) {
    return 0;
  }
  const int size = px > 255 ? 255 : px;
  return static_cast<int16_t>(size);
}

bool XgfFont::blit(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const bool ruby, const bool rotate90,
                   const Plane plane) {
  const uint8_t* slot = slotBytes(bodyId, ruby);
  if (!slot) {
    return false;
  }
  const uint8_t em = ruby ? header.rubyEmPx : header.emPx;
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
  const uint8_t* slot = slotBytes(bodyId, false);
  if (!slot) {
    return false;
  }
  const uint8_t em = header.emPx;
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

bool XgfFont::blitCell(Gfx& gfx, const int x, const int y, const int cellW, const int cellH, const uint16_t bodyId,
                       int px, const bool vertical, const bool bold, const bool rotate90, const bool yakumono,
                       const Plane plane) {
  (void)vertical;
  (void)yakumono;
  if (plane != Plane::Ink) {
    return true;
  }
  if (!opened || cellW < 1 || cellH < 1 || bodyId == 0xFFFF) {
    return false;
  }
  const int em = header.emPx;
  if (em < 1) {
    return false;
  }
  if (px < 1) {
    px = em;
  }
  if (cellW == em && cellH == em && px == em) {
    const bool ok = blit(gfx, x, y, bodyId, false, rotate90, plane);
    if (bold) {
      blit(gfx, x + 1, y, bodyId, false, rotate90, plane);
    }
    return ok;
  }
  const uint8_t* slot = slotBytes(bodyId, false);
  if (!slot) {
    return false;
  }
  const bool crop = cellW <= em && cellH <= em && (cellW == em || cellH == em);
  const int sx0 = crop ? (em - cellW) / 2 : 0;
  const int sy0 = crop ? (em - cellH) / 2 : 0;
  auto sample = [&](const int sx, const int sy) -> bool {
    int ox = sx;
    int oy = sy;
    if (rotate90) {
      ox = sy;
      oy = em - 1 - sx;
    }
    return planeKeep(pixelAt(slot, static_cast<uint8_t>(em), ox, oy), plane);
  };
  for (int dy = 0; dy < cellH; ++dy) {
    for (int dx = 0; dx < cellW; ++dx) {
      bool ink = false;
      if (crop) {
        ink = sample(sx0 + dx, sy0 + dy);
      } else {
        const int x0 = dx * em / cellW;
        const int x1 = (dx + 1) * em / cellW;
        const int y0 = dy * em / cellH;
        const int y1 = (dy + 1) * em / cellH;
        for (int sy = y0; sy < y1 && !ink; ++sy) {
          for (int sx = x0; sx < x1; ++sx) {
            if (sample(sx, sy)) {
              ink = true;
              break;
            }
          }
        }
      }
      if (!ink) {
        continue;
      }
      gfx.drawPixel(x + dx, y + dy, true);
      if (bold) {
        gfx.drawPixel(x + dx + 1, y + dy, true);
      }
    }
  }
  return true;
}

bool XgfFont::blitUi(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const int size, const bool black) {
  if (size <= 0) {
    return false;
  }
  const uint8_t* slot = slotBytes(bodyId, false);
  if (!slot) {
    return false;
  }
  const uint8_t em = header.emPx;
  if (em == 0) {
    return false;
  }
  if (size == static_cast<int>(em)) {
    for (int gy = 0; gy < em; ++gy) {
      for (int gx = 0; gx < em; ++gx) {
        if (pixelAt(slot, em, gx, gy) >= xgf::kInkThreshold) {
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
          if (pixelAt(slot, em, sx, sy) >= xgf::kInkThreshold) {
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
  if (header.emPx > 0 && header.emPx < cell) {
    cell = header.emPx;
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
