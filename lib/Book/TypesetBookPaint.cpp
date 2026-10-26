#include "TypesetBook.h"

#include "Kinsoku.h"
#include "PictureDither.h"
#include "PicturePage.h"

#include <Gfx.h>
#include <Logging.h>
#include <ScratchHeap.h>

#include <cstdio>
#include <cstring>

namespace {

uint32_t fallbackCp(const uint32_t cp) {
  if (cp >= 0x2460 && cp <= 0x2468) {
    return static_cast<uint32_t>('1' + (cp - 0x2460));  // ①–⑨
  }
  if (cp == 0x2469) {
    return static_cast<uint32_t>('0');  // ⑩
  }
  return cp;
}

// Picture sidecar is 1-bit, MSB first. A set bit is black.
bool pictureBlack(const uint8_t* bits, const int pageW, const int x, const int y) {
  const int rowBytes = static_cast<int>(ts::pictureRowBytes(static_cast<uint16_t>(pageW)));
  const uint8_t b = bits[static_cast<size_t>(y) * static_cast<size_t>(rowBytes) + static_cast<size_t>(x >> 3)];
  return (b & static_cast<uint8_t>(0x80u >> (static_cast<unsigned>(x) & 7u))) != 0;
}

// Logical portrait pixels, same 90° clockwise map as Gfx::drawPixel.
void blitPicture(Gfx& gfx, const uint8_t* bits, const int pageW, const int pageH, const ReadFace::Plane plane) {
  uint8_t* fb = gfx.frameBuffer();
  if (!fb || !bits || pageW <= 0 || pageH <= 0 || plane != ReadFace::Plane::Ink) {
    return;
  }
  const int stride = gfx.stride();
  const int pw = gfx.fbWidth();
  const int ph = gfx.fbHeight();
  const int rowBytes = static_cast<int>(ts::pictureRowBytes(static_cast<uint16_t>(pageW)));
  for (int y = 0; y < pageH; ++y) {
    const int phyX = y;
    if (phyX < 0 || phyX >= pw) {
      continue;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << (7 - (phyX & 7)));
    const int col = phyX >> 3;
    const uint8_t* row = bits + static_cast<size_t>(y) * static_cast<size_t>(rowBytes);
    for (int x = 0; x < pageW; ++x) {
      if ((row[x >> 3] & static_cast<uint8_t>(0x80u >> (static_cast<unsigned>(x) & 7u))) == 0) {
        continue;
      }
      const int phyY = ph - 1 - x;
      if (phyY < 0 || phyY >= ph) {
        continue;
      }
      fb[static_cast<size_t>(phyY) * stride + static_cast<size_t>(col)] &= static_cast<uint8_t>(~bit);
    }
  }
}

// Crop the contain-fit rect out of a stored page and scale it into the column box.
void blitPictureBox(Gfx& gfx, const uint8_t* bits, const int pageW, const int pageH, const int srcX, const int srcY,
                    const int srcW, const int srcH, const int dstX, const int dstY, const int dstW, const int dstH,
                    const ReadFace::Plane plane) {
  if (!bits || srcW < 1 || srcH < 1 || dstW < 1 || dstH < 1 || pageW < 4) {
    return;
  }
  for (int dy = 0; dy < dstH; ++dy) {
    int sy = srcY + dy * srcH / dstH;
    if (sy >= pageH) {
      sy = pageH - 1;
    }
    if (sy < 0) {
      continue;
    }
    for (int dx = 0; dx < dstW; ++dx) {
      int sx = srcX + dx * srcW / dstW;
      if (sx < 0 || sx >= pageW) {
        continue;
      }
      if (plane != ReadFace::Plane::Ink || !pictureBlack(bits, pageW, sx, sy)) {
        continue;
      }
      gfx.drawPixel(dstX + dx, dstY + dy, true);
    }
  }
}

// Black pixels in a logical square. Bit 0 is black, same map as Gfx::drawPixel.
uint16_t cellBlack(const Gfx& gfx, const int x, const int y, const int size) {
  const uint8_t* fb = gfx.frameBuffer();
  if (!fb || size <= 0) {
    return 0;
  }
  const int stride = gfx.stride();
  const int pw = gfx.fbWidth();
  const int ph = gfx.fbHeight();
  uint16_t n = 0;
  for (int ly = y; ly < y + size; ++ly) {
    const int phyX = ly;
    if (phyX < 0 || phyX >= pw) {
      continue;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << (7 - (phyX & 7)));
    const int col = phyX >> 3;
    for (int lx = x; lx < x + size; ++lx) {
      const int phyY = ph - 1 - lx;
      if (phyY < 0 || phyY >= ph) {
        continue;
      }
      if ((fb[static_cast<size_t>(phyY) * static_cast<size_t>(stride) + static_cast<size_t>(col)] & bit) == 0) {
        ++n;
      }
    }
  }
  return n;
}

int writeUtf8(const uint32_t cp, char* out) {
  if (cp < 0x21 || cp == 0x7F) {
    out[0] = '_';
    return 1;
  }
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp <= 0xFFFF) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = '?';
  return 1;
}

void pxInk(Gfx& gfx, const int x, const int y) { gfx.drawPixel(x, y, true); }

void inkDot(Gfx& gfx, const int cx, const int cy, const int rad) {
  const int r2 = rad * rad;
  for (int dy = -rad; dy <= rad; ++dy) {
    for (int dx = -rad; dx <= rad; ++dx) {
      if (dx * dx + dy * dy <= r2) {
        pxInk(gfx, cx + dx, cy + dy);
      }
    }
  }
}

void inkRing(Gfx& gfx, const int cx, const int cy, int rad) {
  if (rad < 2) {
    rad = 2;
  }
  int thick = rad / 4;
  if (thick < 1) {
    thick = 1;
  }
  if (thick >= rad) {
    thick = rad - 1;
  }
  const int inner = rad - thick;
  const int outer2 = rad * rad;
  const int inner2 = inner * inner;
  for (int dy = -rad; dy <= rad; ++dy) {
    for (int dx = -rad; dx <= rad; ++dx) {
      const int d2 = dx * dx + dy * dy;
      if (d2 <= outer2 && d2 > inner2) {
        pxInk(gfx, cx + dx, cy + dy);
      }
    }
  }
}

void inkTri(Gfx& gfx, const int cx, const int cy, int rad, const bool pointRight) {
  if (rad < 1) {
    rad = 1;
  }
  for (int dy = -rad; dy <= rad; ++dy) {
    for (int dx = -rad; dx <= rad; ++dx) {
      bool in = false;
      if (pointRight) {
        const int half = (rad - dx) / 2;
        in = dy >= -half && dy <= half;
      } else {
        const int half = (rad + dy) / 2;
        in = dx >= -half && dx <= half;
      }
      if (in) {
        pxInk(gfx, cx + dx, cy + dy);
      }
    }
  }
}

void inkFrame(Gfx& gfx, const int x, const int y, const int rw, const int rh, const int thick) {
  if (rw <= 0 || rh <= 0 || thick <= 0) {
    return;
  }
  if (thick * 2 >= rw || thick * 2 >= rh) {
    gfx.fillRect(x, y, rw, rh, true);
    return;
  }
  gfx.fillRect(x, y, rw, thick, true);
  gfx.fillRect(x, y + rh - thick, rw, thick, true);
  gfx.fillRect(x, y, thick, rh, true);
  gfx.fillRect(x + rw - thick, y, thick, rh, true);
}

int markRad(const int em, const int num, const int den) {
  int r = em * num / den;
  if (r < 1) {
    r = 1;
  }
  return r;
}

void paintMark(Gfx& gfx, const uint8_t kind, const int cx, const int cy, const int em, const bool vert) {
  const int sesame = markRad(em, 1, 10);
  const int dot = markRad(em, 8, 100);
  const int circle = markRad(em, 14, 100);
  switch (kind) {
    case ts::kEmSesameOpen:
      inkRing(gfx, cx, cy, sesame);
      break;
    case ts::kEmDot:
      inkDot(gfx, cx, cy, dot);
      break;
    case ts::kEmDotOpen:
      inkRing(gfx, cx, cy, dot);
      break;
    case ts::kEmCircle:
      inkDot(gfx, cx, cy, circle);
      break;
    case ts::kEmCircleOpen:
      inkRing(gfx, cx, cy, circle);
      break;
    case ts::kEmDoubleCircle:
      inkRing(gfx, cx, cy, circle);
      inkDot(gfx, cx, cy, markRad(circle, 36, 100));
      break;
    case ts::kEmDoubleCircleOpen:
      inkRing(gfx, cx, cy, circle < 3 ? 3 : circle);
      if (circle >= 4) {
        inkRing(gfx, cx, cy, circle * 44 / 100);
      }
      break;
    case ts::kEmTriangle:
      inkTri(gfx, cx, cy, markRad(em, 11, 100), vert);
      break;
    default:
      inkDot(gfx, cx, cy, sesame);
      break;
  }
}

void paintBoxes(Gfx& gfx, const ts::GlyphRun* runs, const uint16_t n, const int fallbackEm) {
  for (uint16_t i = 0; i < n; ++i) {
    const ts::GlyphRun& g = runs[i];
    if (g.box == 0 || ts::runPicture(g)) {
      continue;
    }
    const bool vert = !ts::runRubyAbove(g);
    const int key = vert ? g.x : g.y;
    bool earlier = false;
    for (uint16_t j = 0; j < i; ++j) {
      const ts::GlyphRun& o = runs[j];
      if (o.box != g.box || ts::runPicture(o)) {
        continue;
      }
      const bool ov = !ts::runRubyAbove(o);
      if (ov == vert && (ov ? o.x : o.y) == key) {
        earlier = true;
        break;
      }
    }
    if (earlier) {
      continue;
    }
    int em = g.size > 0 ? static_cast<int>(g.size) : fallbackEm;
    int minX = g.x;
    int minY = g.y;
    int maxX = g.x + (vert ? (g.size > 0 ? static_cast<int>(g.size) : em) : ts::runAdvance(g));
    int maxY = g.y + (vert ? ts::runAdvance(g) : (g.size > 0 ? static_cast<int>(g.size) : em));
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < n; ++j) {
      const ts::GlyphRun& o = runs[j];
      if (o.box != g.box || ts::runPicture(o)) {
        continue;
      }
      const bool ov = !ts::runRubyAbove(o);
      if (ov != vert || (ov ? o.x : o.y) != key) {
        continue;
      }
      const int oem = o.size > 0 ? static_cast<int>(o.size) : em;
      if (o.x < minX) {
        minX = o.x;
      }
      if (o.y < minY) {
        minY = o.y;
      }
      const int ox = o.x + (ov ? (o.size > 0 ? static_cast<int>(o.size) : oem) : ts::runAdvance(o));
      const int oy = o.y + (ov ? ts::runAdvance(o) : (o.size > 0 ? static_cast<int>(o.size) : oem));
      if (ox > maxX) {
        maxX = ox;
      }
      if (oy > maxY) {
        maxY = oy;
      }
      if (oem > em) {
        em = oem;
      }
    }
    int outset = em * 12 / 100;
    if (outset < 1) {
      outset = 1;
    }
    int thick = em * 6 / 100;
    if (thick < 1) {
      thick = 1;
    }
    inkFrame(gfx, minX - outset, minY - outset, (maxX - minX) + outset * 2, (maxY - minY) + outset * 2, thick);
  }
}

}  // namespace

void TypesetBook::paint(Gfx& gfx, const ReadFace::Plane plane) {
  // Black on white. Gray planes stay empty.
  gfx.clear(plane != ReadFace::Plane::Ink);
  if (plane != ReadFace::Plane::Ink) {
    return;
  }
  if (loadedPicture != 0) {
    if (pictureBits) {
      blitPicture(gfx, pictureBits, layoutOpt.width, layoutOpt.height, plane);
    }
    return;
  }
  const int em = font.emPx();
  const int rubyEm = font.rubyEmPx();
  const uint16_t pageW = static_cast<uint16_t>(layoutOpt.width);
  const uint16_t pageH = static_cast<uint16_t>(layoutOpt.height);
  auto paintInline = [&](const ts::GlyphRun& g) {
    const uint16_t id = static_cast<uint16_t>(g.cp > 0xFFFFu ? 0xFFFFu : g.cp);
    if (id == 0 || pageW == 0 || pageH == 0) {
      return;
    }
    const size_t nbytes = ts::pictureRowBytes(pageW) * pageH;
    if (scratchPic != id || !pictureBits) {
      uint8_t* dst = ScratchHeap::data();
      if (!dst || ScratchHeap::size() < nbytes ||
          !ts::pictureRead(picturePath, id, pageW, pageH, dst, nbytes)) {
        return;
      }
      pictureBits = dst;
      scratchPic = id;
    }
    ts::FitBox fit = ts::containFit(static_cast<uint16_t>(g.ruby[0] > 65535u ? 65535u : g.ruby[0]),
                                    static_cast<uint16_t>(g.ruby[1] > 65535u ? 65535u : g.ruby[1]), pageW, pageH);
    if (fit.w == 0 || fit.h == 0) {
      fit.x = 0;
      fit.y = 0;
      fit.w = pageW;
      fit.h = pageH;
    }
    const int dw = g.size > 0 ? static_cast<int>(g.size) : 1;
    const int dh = g.advance > 0 ? static_cast<int>(g.advance) : 1;
    blitPictureBox(gfx, pictureBits, pageW, pageH, fit.x, fit.y, fit.w, fit.h, g.x, g.y, dw, dh, plane);
  };
  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    if (ts::runPicture(g)) {
      paintInline(g);
      continue;
    }
    const bool vert = !ts::runRubyAbove(g);
    const int adv = ts::runAdvance(g);
    const bool tcyRun = ts::runTcy(g) && g.rubyCount > 1;

    if (tcyRun) {
      const int n = g.rubyCount;
      int size = em / n;
      if (size < 1) {
        size = 1;
      }
      const int y0 = g.y + (em - size) / 2;
      const int x0 = g.x + (em - size * n) / 2;
      for (int r = 0; r < n; ++r) {
        uint16_t id = font.glyphId(g.ruby[r]);
        if (id == 0xFFFF) {
          id = font.glyphId(fallbackCp(g.ruby[r]));
        }
        if (id != 0xFFFF) {
          if ((g.flags & ts::kRunBold) != 0) {
            font.blitCell(gfx, x0 + r * size, y0, size, size, id, size, false, true, false, false, plane);
          } else {
            font.blitBox(gfx, x0 + r * size, y0, id, size, plane);
          }
        }
      }
    } else {
      uint16_t id = font.glyphId(g.cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(g.cp));
      }
      if (id != 0xFFFF) {
        const bool bold = (g.flags & ts::kRunBold) != 0;
        const bool half = (g.flags & ts::kRunHalfCell) != 0;
        const int face = g.size > 0 ? static_cast<int>(g.size) : em;
        const bool proportional = adv != face;
        if (half || bold || face != em || proportional) {
          int cellW = vert ? face : adv;
          int cellH = vert ? adv : face;
          if (cellW < 1) {
            cellW = 1;
          }
          if (cellH < 1) {
            cellH = 1;
          }
          font.blitCell(gfx, g.x, g.y, cellW, cellH, id, face, vert, bold, ts::runRotate90(g), ts::kinsokuYakumono(g.cp),
                        plane);
        } else {
          font.blit(gfx, g.x, g.y, id, false, ts::runRotate90(g), plane);
        }
      }
    }

    if (g.emphasis != 0 && em > 0) {
      const bool hasRuby = !ts::runTcy(g) && g.rubyCount > 0;
      int gap = em / (hasRuby ? 10 : 4);
      if (gap < 1) {
        gap = 1;
      }
      if (g.emphasis == ts::kEmLine) {
        int thick = em / 14;
        if (thick < 1) {
          thick = 1;
        }
        if (vert) {
          gfx.fillRect(g.x + em + gap, g.y, thick, adv, true);
        } else {
          gfx.fillRect(g.x, g.y - gap - thick, adv, thick, true);
        }
      } else {
        const int cx = vert ? g.x + em + gap : g.x + adv / 2;
        const int cy = vert ? g.y + adv / 2 : g.y - gap;
        paintMark(gfx, g.emphasis, cx, cy, em, vert);
      }
    }

    if (tcyRun || g.rubyCount == 0 || rubyEm == 0) {
      continue;
    }

    const int lead = g.rubyLead;
    const int origin = (vert ? g.y : g.x) - lead;
    int lastEnd = (vert ? g.y : g.x) + adv;
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < loadedCount; ++j) {
      const ts::GlyphRun& o = loadedGlyphs[j];
      if (!ts::runSticky(o)) {
        break;
      }
      if (vert) {
        if (o.x != g.x) {
          break;
        }
      } else if (o.y != g.y) {
        break;
      }
      lastEnd = (vert ? o.y : o.x) + ts::runAdvance(o);
    }
    const int span = lastEnd - origin + lead;
    if (span <= 0) {
      continue;
    }
    const int clusterLo = origin;
    const int clusterHi = origin + span;
    auto inside = [&](const ts::GlyphRun& o) -> bool {
      if (vert) {
        return o.x == g.x && o.y >= clusterLo && o.y < clusterHi;
      }
      return o.y == g.y && o.x >= clusterLo && o.x < clusterHi;
    };
    auto blocks = [&](const int dir) -> bool {
      const ts::GlyphRun* best = nullptr;
      int bestAbs = 0;
      for (uint16_t j = 0; j < loadedCount; ++j) {
        const ts::GlyphRun& o = loadedGlyphs[j];
        if (inside(o)) {
          continue;
        }
        if (vert) {
          if (o.x != g.x) {
            continue;
          }
        } else if (o.y != g.y) {
          continue;
        }
        const int d = (vert ? o.y : o.x) - origin;
        if ((dir < 0 && d >= 0) || (dir > 0 && d <= 0)) {
          continue;
        }
        const int ad = d < 0 ? -d : d;
        if (!best || ad < bestAbs) {
          best = &o;
          bestAbs = ad;
        }
      }
      return best && best->rubyCount > 0 && bestAbs <= span;
    };
    const ts::RubyAlong along = ts::placeRubyAlong(static_cast<int16_t>(origin), static_cast<int16_t>(span),
                                                   static_cast<int16_t>(rubyEm), g.rubyCount, !blocks(-1), !blocks(1));
    const int beside = g.emphasis ? em * 28 / 100 : 0;
    for (uint8_t r = 0; r < g.rubyCount; ++r) {
      uint16_t rid = font.glyphId(g.ruby[r]);
      if (rid == 0xFFFF) {
        rid = font.glyphId(fallbackCp(g.ruby[r]));
      }
      if (rid == 0xFFFF) {
        continue;
      }
      const int at = along.start + r * along.pitch;
      const int rx = vert ? g.x + em + beside : at;
      const int ry = vert ? at : g.y - rubyEm - beside;
      font.blit(gfx, rx, ry, rid, true, false, plane);
    }
  }
  paintBoxes(gfx, loadedGlyphs, loadedCount, em);
}

void TypesetBook::logPaintedGlyphs(Gfx& gfx, const uint32_t pageIndex) const {
  const ReadFace::CacheStats st = font.cacheStats();
  if (loadedPicture != 0) {
    LOG_INF("TS", "gly p%lu picture=%u", static_cast<unsigned long>(pageIndex + 1), loadedPicture);
    return;
  }
  LOG_INF("TS", "gly p%lu runs=%u em=%u body=%u/%u ruby=%u/%u hit=%lu miss=%lu ff=%lu blk=%lu",
          static_cast<unsigned long>(pageIndex + 1), loadedCount, font.emPx(), st.bodyUsed, st.bodyCap, st.rubyUsed,
          st.rubyCap, static_cast<unsigned long>(st.hits), static_cast<unsigned long>(st.misses),
          static_cast<unsigned long>(st.rejected), static_cast<unsigned long>(st.blocked));

  // One token per codepoint: char+cp id hash sourceInk/drawnBlack flags.
  // B = cached bitmap is mostly ink. D = the cell is much blacker than the
  // bitmap. M = not in the cache, so the cell was left blank. r = ruby.
  static uint32_t seen[160];
  uint16_t nSeen = 0;
  char line[160];
  size_t used = 0;
  auto fresh = [&](const uint32_t key) -> bool {
    for (uint16_t i = 0; i < nSeen; ++i) {
      if (seen[i] == key) {
        return false;
      }
    }
    if (nSeen < 160) {
      seen[nSeen++] = key;
    }
    return true;
  };
  auto flush = [&]() {
    if (used == 0) {
      return;
    }
    line[used] = '\0';
    LOG_INF("TS", "gly %s", line);
    used = 0;
  };
  auto add = [&](const char* tok) {
    const size_t n = strlen(tok);
    if (used != 0 && used + 1 + n >= sizeof(line)) {
      flush();
    }
    if (used != 0 && used + 1 < sizeof(line)) {
      line[used++] = ' ';
    }
    if (n >= sizeof(line)) {
      return;
    }
    memcpy(line + used, tok, n);
    used += n;
  };
  auto token = [&](const uint32_t cp, const uint16_t id, const bool ruby, const int x, const int y, const int size,
                   const uint8_t flags, const bool measure) {
    char utf[4];
    const int un = writeUtf8(cp, utf);
    ReadFace::SlotProbe pr;
    const bool ok = id != 0xFFFF && font.probe(id, ruby, pr);
    const uint16_t dst = measure ? cellBlack(gfx, x, y, size) : 0;
    char mark[4] = {};
    size_t m = 0;
    if (!ok) {
      mark[m++] = 'M';
    } else if (pr.area != 0 && static_cast<uint32_t>(pr.ink) * 100u / pr.area >= 55u) {
      mark[m++] = 'B';
    }
    if (measure && ok && dst > static_cast<uint16_t>(pr.ink + pr.ink / 3 + 40)) {
      mark[m++] = 'D';
    }
    char tok[72];
    if (measure) {
      snprintf(tok, sizeof(tok), "%s%.*s+%04lX/%04X/%08lX/%u/%u/%02X%s", ruby ? "r" : "", un, utf,
               static_cast<unsigned long>(cp), id, static_cast<unsigned long>(ok ? pr.hash : 0), ok ? pr.ink : 0, dst,
               flags, mark);
    } else {
      snprintf(tok, sizeof(tok), "%s%.*s+%04lX/%04X/%08lX/%u/-/%02X%s", ruby ? "r" : "", un, utf,
               static_cast<unsigned long>(cp), id, static_cast<unsigned long>(ok ? pr.hash : 0), ok ? pr.ink : 0, flags,
               mark);
    }
    add(tok);
  };

  const int em = font.emPx();
  const int rubyEm = font.rubyEmPx();
  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    if (ts::runPicture(g)) {
      continue;
    }
    const bool tcyRun = ts::runTcy(g) && g.rubyCount > 1;
    if (tcyRun) {
      const int n = g.rubyCount;
      int size = em / n;
      if (size < 1) {
        size = 1;
      }
      const int y0 = g.y + (em - size) / 2;
      const int x0 = g.x + (em - size * n) / 2;
      for (int r = 0; r < n; ++r) {
        const uint32_t cp = g.ruby[r];
        if (!fresh(cp | 0x40000000u)) {
          continue;
        }
        uint16_t id = font.glyphId(cp);
        if (id == 0xFFFF) {
          id = font.glyphId(fallbackCp(cp));
        }
        token(cp, id, false, x0 + r * size, y0, size, g.flags, false);
      }
      continue;
    }

    const int drawX = g.x;
    const int drawY = g.y;
    const int box = (g.flags & ts::kRunHalfCell) != 0 ? ts::runAdvance(g) : em;
    if (fresh(g.cp)) {
      uint16_t id = font.glyphId(g.cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(g.cp));
      }
      token(g.cp, id, false, drawX, drawY, box, g.flags, true);
    }
    if (g.rubyCount == 0 || rubyEm == 0) {
      continue;
    }
    for (uint8_t r = 0; r < g.rubyCount; ++r) {
      const uint32_t cp = g.ruby[r];
      if (!fresh(cp | 0x80000000u)) {
        continue;
      }
      uint16_t id = font.glyphId(cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(cp));
      }
      token(cp, id, true, 0, 0, rubyEm, g.flags, false);
    }
  }
  flush();
}

bool TypesetBook::drawPage(Gfx& gfx, const uint32_t pageIndex, int& pagesUntilFullRefresh,
                          const int refreshFrequency) {
  if (!opened) {
    error = "closed";
    return false;
  }
  const unsigned long tDraw = millis();
  const ReadFace::CacheStats sd0 = font.cacheStats();
  const unsigned long tRebase = millis();
  flushPendingCleanup(gfx);
  const unsigned long rebaseMs = millis() - tRebase;
  const unsigned long tLay = millis();
  if (!layoutPage(pageIndex)) {
    return false;
  }
  const unsigned long layMs = millis() - tLay;
  // SD and the panel share SPI. Read the picture before the refresh starts.
  const unsigned long tPic = millis();
  pictureBits = nullptr;
  scratchPic = 0;
  if (loadedPicture != 0) {
    const uint16_t pw = static_cast<uint16_t>(layoutOpt.width);
    const uint16_t ph = static_cast<uint16_t>(layoutOpt.height);
    const size_t nbytes = ts::pictureRowBytes(pw) * ph;
    uint8_t* dst = ScratchHeap::data();
    if (dst && ScratchHeap::size() >= nbytes && ts::pictureRead(picturePath, loadedPicture, pw, ph, dst, nbytes)) {
      pictureBits = dst;
      scratchPic = loadedPicture;
    } else {
      LOG_ERR("TS", "picture %u missing", loadedPicture);
    }
  }
  const unsigned long picMs = millis() - tPic;
  const unsigned long tInk = millis();
  paint(gfx, ReadFace::Plane::Ink);
  const unsigned long inkMs = millis() - tInk;
  // After the displayed ink frame, before the refresh takes the SPI bus.
  const unsigned long tGly = millis();
  logPaintedGlyphs(gfx, pageIndex);
  const unsigned long glyMs = millis() - tGly;

  const unsigned long tBase = millis();
  if (pagesUntilFullRefresh <= 1) {
    gfx.present(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = refreshFrequency;
  } else {
    gfx.present(HalDisplay::FAST_REFRESH);
    --pagesUntilFullRefresh;
  }
  const unsigned long presentMs = millis() - tBase;
  const ReadFace::CacheStats sd1 = font.cacheStats();
  // Milliseconds except sd reads/bytes and the hit/miss counts. spi is the
  // black-and-white refresh.
  LOG_INF("TS",
          "perf p%lu lay %lu pic %lu ink %lu gly %lu rebase %lu spi %lu sd %lu/%lu/%lu "
          "hit+%lu miss+%lu body %u/%u total %lu",
          static_cast<unsigned long>(pageIndex + 1), layMs, picMs, inkMs, glyMs, rebaseMs, presentMs,
          static_cast<unsigned long>(sd1.sdReads - sd0.sdReads),
          static_cast<unsigned long>(sd1.sdBytes - sd0.sdBytes),
          static_cast<unsigned long>((sd1.sdUs - sd0.sdUs) / 1000u), static_cast<unsigned long>(sd1.hits - sd0.hits),
          static_cast<unsigned long>(sd1.misses - sd0.misses), sd1.bodyUsed, sd1.bodyCap, millis() - tDraw);
  error = "";
  return true;
}

void TypesetBook::prefetchForward(const uint32_t fromPageIndex) {
  if (!opened || fromPageIndex + 1 >= nPages) {
    return;
  }
  const unsigned long t0 = millis();
  const ReadFace::CacheStats sd0 = font.cacheStats();
  (void)layoutPage(fromPageIndex + 1);
  const ReadFace::CacheStats sd1 = font.cacheStats();
  LOG_INF("TS", "perf prefetch p%lu %lums sd %lu/%lu/%lu miss+%lu", static_cast<unsigned long>(fromPageIndex + 2),
          millis() - t0, static_cast<unsigned long>(sd1.sdReads - sd0.sdReads),
          static_cast<unsigned long>(sd1.sdBytes - sd0.sdBytes),
          static_cast<unsigned long>((sd1.sdUs - sd0.sdUs) / 1000u),
          static_cast<unsigned long>(sd1.misses - sd0.misses));
}

void TypesetBook::flushPendingCleanup(Gfx& gfx) {
  if (!cleanupPending) {
    return;
  }
  gfx.cleanupGrayscaleBuffers();
  cleanupPending = false;
}
