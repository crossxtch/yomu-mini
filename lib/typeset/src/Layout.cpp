#include "Layout.h"

#include "FaceCss.h"
#include "Kinsoku.h"

#include <algorithm>

namespace ts {
namespace {

void copyRuby(GlyphRun& run, const uint32_t* ruby, const uint8_t rubyCount) {
  const uint8_t n = rubyCount > kMaxCluster ? kMaxCluster : rubyCount;
  run.rubyCount = n;
  for (uint8_t i = 0; i < n; ++i) {
    run.ruby[i] = ruby ? ruby[i] : 0;
  }
}

// 0 and 100 are the body em. Anything else is a percent of it, clamped to a pixel size.
int16_t facePx(const LayoutOptions& opt, const uint8_t scale) {
  if (scale == 0 || scale == 100 || opt.em < 1) {
    return opt.em < 1 ? static_cast<int16_t>(1) : static_cast<int16_t>(opt.em);
  }
  int px = static_cast<int>(opt.em) * static_cast<int>(scale) / 100;
  if (px < 1) {
    px = 1;
  }
  if (px > 255) {
    px = 255;
  }
  return static_cast<int16_t>(px);
}

}  // namespace

void PageLayouter::begin(const LayoutOptions& options) {
  opt = options;
  if (opt.em < 1) {
    opt.em = 1;
  }
  currentCount = 0;
  committedCount = 0;
  pageReady = false;
  lastAtomPos = 0;
  firstOnCurrent = 0;
  feedPos = 0;
  takenId = 0;
  deferredId = 0;
  columnClosed = false;
  if (!recomputeGrid()) {
    return;
  }
  resetCursor();
}

bool PageLayouter::recomputeGrid() {
  rubyGutter = opt.compactColumns && !opt.hasRuby ? static_cast<int16_t>(std::max(1, opt.em / 5))
                                                  : static_cast<int16_t>((opt.em + 1) / 2);
  pitch = static_cast<int16_t>(opt.em + rubyGutter);
  const int16_t innerW = opt.width;
  int16_t marginY = opt.margin;
  if (marginY < 0) {
    marginY = 0;
  }
  if (opt.height - marginY * 2 < 1) {
    marginY = 0;
  }
  const int16_t innerH = static_cast<int16_t>(opt.height - marginY * 2);

  if (opt.mode == WritingMode::VerticalRl) {
    const int nCols = pitch > 0 ? innerW / pitch : 0;
    const int nChars = opt.em > 0 ? innerH / opt.em : 0;
    if (nCols < 1 || nChars < 1) {
      return false;
    }
    const int16_t usedW = static_cast<int16_t>(nCols * pitch);
    const int16_t usedH = static_cast<int16_t>(nChars * opt.em);
    const int16_t padLeft = static_cast<int16_t>((innerW - usedW) / 2);
    const int16_t padTop = static_cast<int16_t>((innerH - usedH) / 2);
    colTop = static_cast<int16_t>(marginY + padTop);
    colBottom = static_cast<int16_t>(colTop + usedH);
    hanmenLeft = padLeft;
    firstColX = static_cast<int16_t>(hanmenLeft + usedW - pitch);
  } else {
    const int nChars = opt.em > 0 ? innerW / opt.em : 0;
    const int nLines = pitch > 0 ? innerH / pitch : 0;
    if (nChars < 1 || nLines < 1) {
      return false;
    }
    const int16_t usedW = static_cast<int16_t>(nChars * opt.em);
    const int16_t usedH = static_cast<int16_t>(nLines * pitch);
    const int16_t padLeft = static_cast<int16_t>((innerW - usedW) / 2);
    const int16_t padTop = static_cast<int16_t>((innerH - usedH) / 2);
    firstLineY = static_cast<int16_t>(marginY + padTop);
    hanmenLeft = padLeft;
    lineRight = static_cast<int16_t>(hanmenLeft + usedW);
    hanmenBottom = static_cast<int16_t>(firstLineY + usedH);
  }
  return true;
}

int16_t PageLayouter::charAdvance(const uint32_t cp, const bool tcy, const int16_t em) const {
  const int16_t grid = kinsokuInlineAdvance(cp, em, tcy);
  if (tcy || opt.mode != WritingMode::HorizontalTb || grid < em || usesEmAdvance(cp) || !opt.measure) {
    return grid;
  }
  const int16_t w = opt.measure(opt.measureCtx, cp, em);
  return w > 0 ? w : grid;
}

int16_t PageLayouter::bracketGap(const uint32_t cp, const int16_t em) const {
  if (columnClosed || currentCount == 0) {
    return 0;
  }
  const GlyphRun& prev = current[currentCount - 1];
  if (opt.mode == WritingMode::VerticalRl) {
    if (prev.x != colX) {
      return 0;
    }
  } else if (prev.y != glyphY()) {
    return 0;
  }
  const int16_t basis = prev.size > 0 ? static_cast<int16_t>(prev.size) : em;
  return kinsokuBracketPairGap(prev.cp, cp, basis);
}

void PageLayouter::resetCursor() {
  columnClosed = false;
  lastBox = 0;
  if (opt.mode == WritingMode::VerticalRl) {
    indent = colTop;
    colX = firstColX;
    y = indent;
  } else {
    indent = hanmenLeft;
    lineY = firstLineY;
    x = indent;
  }
}

void PageLayouter::emit(const GlyphRun& run, const uint32_t pos) {
  if (currentCount >= kMaxGlyphs) {
    newPage();
  }
  if (currentCount == 0) {
    firstOnCurrent = pos;
  }
  if (currentCount < kMaxGlyphs) {
    current[currentCount++] = run;
  }
}

void PageLayouter::newPage() {
  committedCount = currentCount;
  for (uint16_t i = 0; i < currentCount; ++i) {
    committed[i] = current[i];
  }
  currentCount = 0;
  firstOnCurrent = 0;
  resetCursor();
  pageReady = true;
}

void PageLayouter::newColumn(const int extra) {
  columnClosed = false;
  colX = static_cast<int16_t>(colX - pitch - extra);
  y = indent;
  if (colX < hanmenLeft) {
    newPage();
  }
}

void PageLayouter::newLine(const int extra) {
  columnClosed = false;
  lineY = static_cast<int16_t>(lineY + pitch + extra);
  x = indent;
  if (lineY + pitch > hanmenBottom) {
    newPage();
  }
}

uint16_t PageLayouter::stickyTail(const bool vertical) const {
  if (currentCount == 0) {
    return 0;
  }
  const GlyphRun& last = current[currentCount - 1];
  if (vertical) {
    if (last.x != colX) {
      return 0;
    }
  } else if (last.y != glyphY()) {
    return 0;
  }
  uint16_t n = 1;
  while (n < currentCount && n < 8) {
    const GlyphRun& g = current[currentCount - n];
    if (!runSticky(g)) {
      break;
    }
    const GlyphRun& prev = current[currentCount - n - 1];
    if (vertical) {
      if (prev.x != colX) {
        break;
      }
    } else if (prev.y != glyphY()) {
      break;
    }
    ++n;
  }
  return n;
}

void PageLayouter::relocateTail(const uint16_t nIn, const bool vertical) {
  if (nIn == 0 || currentCount < nIn) {
    return;
  }
  const uint16_t n = nIn > 8 ? 8 : nIn;
  GlyphRun tail[8];
  const uint16_t base = static_cast<uint16_t>(currentCount - n);
  for (uint16_t i = 0; i < n; ++i) {
    tail[i] = current[base + i];
  }
  currentCount = base;
  if (currentCount == 0) {
    firstOnCurrent = 0;
  }
  if (vertical) {
    newColumn();
  } else {
    newLine();
  }
  const int16_t lead = tail[0].rubyLead;
  const int16_t firstAt = vertical ? tail[0].y : tail[0].x;
  const int16_t origin = static_cast<int16_t>(firstAt - lead);
  const int16_t lastEnd = static_cast<int16_t>((vertical ? tail[n - 1].y : tail[n - 1].x) + runAdvance(tail[n - 1]));
  const int16_t span = static_cast<int16_t>(lastEnd - origin + lead);
  const int16_t top = vertical ? y : x;
  for (uint16_t i = 0; i < n; ++i) {
    GlyphRun g = tail[i];
    const int16_t at = static_cast<int16_t>(top + ((vertical ? tail[i].y : tail[i].x) - origin));
    if (vertical) {
      g.x = colX;
      g.y = at;
    } else {
      g.x = at;
      g.y = glyphY();
    }
    emit(g, lastAtomPos);
  }
  if (vertical) {
    y = static_cast<int16_t>(top + span);
  } else {
    x = static_cast<int16_t>(top + span);
  }
}

namespace {

void fitBox(const int sw, const int sh, int maxW, int maxH, int& dw, int& dh) {
  if (maxW < 1) {
    maxW = 1;
  }
  if (maxH < 1) {
    maxH = 1;
  }
  dw = sw > 0 ? sw : 1;
  dh = sh > 0 ? sh : 1;
  if (dw > maxW || dh > maxH) {
    if (static_cast<long long>(dw) * maxH <= static_cast<long long>(dh) * maxW) {
      dh = maxH;
      dw = static_cast<int>(static_cast<long long>(sw) * maxH / (sh > 0 ? sh : 1));
    } else {
      dw = maxW;
      dh = static_cast<int>(static_cast<long long>(sh) * maxW / (sw > 0 ? sw : 1));
    }
  }
  if (dw < 1) {
    dw = 1;
  }
  if (dh < 1) {
    dh = 1;
  }
  if (dw > maxW) {
    dw = maxW;
  }
  if (dh > maxH) {
    dh = maxH;
  }
}

/* A plate covers at least half the short side of the page. Anything smaller
 * shares the column with the text. */
bool staysInColumn(const int pxW, const int pxH, const int pageW, const int pageH) {
  if (pxW < 1 || pxH < 1 || pageW < 1 || pageH < 1) {
    return false;
  }
  const int longer = pxW > pxH ? pxW : pxH;
  const int shorter = pageW < pageH ? pageW : pageH;
  return longer * 2 <= shorter;
}

}  // namespace

void PageLayouter::placePicture(const uint16_t id, const int pxW, const int pxH) {
  const int em = opt.em > 0 ? opt.em : 1;
  const bool vert = opt.mode == WritingMode::VerticalRl;
  int bw = 1;
  int bh = 1;
  if (vert) {
    if (columnClosed) {
      newColumn();
    }
    const int colH = colBottom > colTop ? colBottom - colTop : 1;
    fitBox(pxW, pxH, em, colH, bw, bh);
    if (y > indent && static_cast<int>(y) + bh > colBottom) {
      newColumn();
    }
    const int room = colBottom - y;
    if (room > 0 && bh > room) {
      fitBox(pxW, pxH, em, room, bw, bh);
    }
    int x = static_cast<int>(colX) + (em - bw) / 2;
    if (x < colX) {
      x = colX;
    }
    GlyphRun run{};
    run.cp = id;
    run.size = static_cast<uint16_t>(bw);
    run.advance = static_cast<uint16_t>(bh);
    run.flags = kRunPicture;
    run.ruby[0] = static_cast<uint32_t>(pxW > 0 ? pxW : 0);
    run.ruby[1] = static_cast<uint32_t>(pxH > 0 ? pxH : 0);
    run.x = static_cast<int16_t>(x);
    run.y = y;
    emit(run, feedPos);
    y = static_cast<int16_t>(y + bh);
    return;
  }
  if (columnClosed) {
    newLine();
  }
  const int lineW = lineRight > hanmenLeft ? lineRight - hanmenLeft : 1;
  fitBox(pxW, pxH, lineW, em, bw, bh);
  if (x > indent && static_cast<int>(x) + bw > lineRight) {
    newLine();
  }
  const int room = lineRight - x;
  if (room > 0 && bw > room) {
    fitBox(pxW, pxH, room, em, bw, bh);
  }
  int py = static_cast<int>(glyphY()) + (em - bh) / 2;
  if (py < glyphY()) {
    py = glyphY();
  }
  GlyphRun run{};
  run.cp = id;
  run.size = static_cast<uint16_t>(bw);
  run.advance = static_cast<uint16_t>(bh);
  run.flags = kRunPicture;
  run.ruby[0] = static_cast<uint32_t>(pxW > 0 ? pxW : 0);
  run.ruby[1] = static_cast<uint32_t>(pxH > 0 ? pxH : 0);
  run.x = x;
  run.y = static_cast<int16_t>(py);
  emit(run, feedPos);
  x = static_cast<int16_t>(x + bw);
}

void PageLayouter::padBox(const uint8_t box, const int16_t em) {
  const uint8_t prev = lastBox;
  if (prev == box) {
    return;
  }
  const bool atStart = opt.mode == WritingMode::VerticalRl ? y <= indent : x <= indent;
  if (atStart && prev == 0) {
    lastBox = box;
    return;
  }
  const int pad = static_cast<int>(em) * 22 / 100;
  if (pad > 0) {
    if (opt.mode == WritingMode::VerticalRl) {
      if (static_cast<int>(y) + pad <= colBottom) {
        y = static_cast<int16_t>(y + pad);
      }
    } else if (static_cast<int>(x) + pad <= lineRight) {
      x = static_cast<int16_t>(x + pad);
    }
  }
  lastBox = box;
}

void PageLayouter::placeChar(const uint32_t cp, const uint32_t* ruby, const uint8_t rubyCount, const bool tcy,
                             const uint8_t emphasis, const uint32_t* tcyCps, const uint8_t tcyN, const uint8_t scale,
                             const uint8_t bold, const uint8_t box, const uint8_t shade, const uint8_t rulePx,
                             const uint8_t ruleEm, const uint8_t ruleRoot, const uint8_t ruleSides, const uint8_t sans) {
  const int16_t F = facePx(opt, scale);
  padBox(box, F);
  int16_t h = charAdvance(cp, tcy, F);
  const bool vert = opt.mode == WritingMode::VerticalRl;
  if (!vert && !tcy && ruby && rubyCount > 0) {
    const int16_t rubyW = static_cast<int16_t>(rubyCount * (F / 2 > 0 ? F / 2 : 1));
    if (rubyW > h) {
      h = rubyW;
    }
  }

  auto fill = [&](GlyphRun& run) {
    run.cp = cp;
    run.size = static_cast<uint16_t>(F);
    run.advance = static_cast<uint16_t>(h > 0 ? h : 0);
    run.emphasis = static_cast<uint8_t>(emphasis & kEmphasisMask);
    run.box = box;
    run.shade = shade;
    run.rulePx = rulePx;
    run.ruleEm = ruleEm;
    run.ruleRoot = ruleRoot;
    run.ruleSides = ruleSides;
    if (bold) {
      run.flags = static_cast<uint8_t>(run.flags | kRunBold);
    }
    if (sans) {
      run.flags = static_cast<uint8_t>(run.flags | kRunGothic);
    }
    if (tcy && tcyCps && tcyN > 0) {
      copyRuby(run, tcyCps, tcyN);
      run.cp = tcyCps[0];
      run.flags = static_cast<uint8_t>(run.flags | kRunTcy);
    } else if (ruby && rubyCount > 0) {
      copyRuby(run, ruby, rubyCount);
    }
    if (h < F && kinsokuHalfAdvance(cp)) {
      run.flags = static_cast<uint8_t>(run.flags | kRunHalfCell);
    }
  };

  if (vert) {
    if (columnClosed) {
      newColumn();
    }
    if (!tcy && kinsokuHangs(cp) && y > indent && y <= colBottom && static_cast<int>(y) + h > colBottom) {
      GlyphRun run{};
      fill(run);
      run.x = colX;
      run.y = y;
      if (shouldRotate(cp)) {
        run.flags = static_cast<uint8_t>(run.flags | kRunRotate);
      }
      emit(run, feedPos);
      columnClosed = true;
      return;
    }
    if (static_cast<int>(y) + h > colBottom) {
      const bool startKinsoku = !kinsokuCanStartColumn(cp);
      const bool endKinsoku = currentCount > 0 && !kinsokuCanEndColumn(current[currentCount - 1].cp);
      const uint16_t tail = stickyTail(true);
      if ((startKinsoku || endKinsoku) && tail > 0) {
        relocateTail(tail, true);
      } else {
        newColumn();
      }
    }
    if (static_cast<int>(y) + h > colBottom) {
      newColumn();
    }
    int16_t gap = tcy ? static_cast<int16_t>(0) : bracketGap(cp, F);
    if (gap > 0 && static_cast<int>(y) + gap + h > colBottom && y > indent) {
      newColumn();
      gap = 0;
    }
    if (gap > 0) {
      y = static_cast<int16_t>(y + gap);
    }
    GlyphRun run{};
    fill(run);
    run.x = colX;
    run.y = y;
    if (!tcy && shouldRotate(cp)) {
      run.flags = static_cast<uint8_t>(run.flags | kRunRotate);
    }
    emit(run, feedPos);
    y = static_cast<int16_t>(y + h);
    return;
  }

  if (columnClosed) {
    newLine();
  }
  if (!tcy && kinsokuHangs(cp) && x > indent && x <= lineRight && static_cast<int>(x) + h > lineRight) {
    GlyphRun run{};
    fill(run);
    run.x = x;
    run.y = glyphY();
    run.flags = static_cast<uint8_t>(run.flags | kRunRubyAbove);
    emit(run, feedPos);
    columnClosed = true;
    return;
  }
  if (static_cast<int>(x) + h > lineRight) {
    const bool startKinsoku = !kinsokuCanStartColumn(cp);
    const bool endKinsoku = currentCount > 0 && !kinsokuCanEndColumn(current[currentCount - 1].cp);
    const uint16_t tail = stickyTail(false);
    if ((startKinsoku || endKinsoku) && tail > 0) {
      relocateTail(tail, false);
    } else {
      newLine();
    }
  }
  if (static_cast<int>(x) + h > lineRight) {
    newLine();
  }
  int16_t gap = tcy ? static_cast<int16_t>(0) : bracketGap(cp, F);
  if (gap > 0 && static_cast<int>(x) + gap + h > lineRight && x > indent) {
    newLine();
    gap = 0;
  }
  if (gap > 0) {
    x = static_cast<int16_t>(x + gap);
  }
  GlyphRun run{};
  fill(run);
  run.x = x;
  run.y = glyphY();
  run.flags = static_cast<uint8_t>(run.flags | kRunRubyAbove);
  emit(run, feedPos);
  x = static_cast<int16_t>(x + h);
}

void PageLayouter::placeGroup(const Atom& atom) {
  const int16_t F = facePx(opt, atom.startEm);
  const bool bold = (atom.emphasis & kEmphasisBold) != 0;
  const uint8_t mark = static_cast<uint8_t>(atom.emphasis & kEmphasisMask);
  const bool vert = opt.mode == WritingMode::VerticalRl;
  padBox(atom.box, F);
  const uint8_t nBase = atom.tcyCount > kMaxCluster ? kMaxCluster : atom.tcyCount;
  if (nBase == 0) {
    return;
  }
  const uint8_t nRuby = atom.rubyCount > kMaxCluster ? kMaxCluster : atom.rubyCount;
  int16_t advs[kMaxCluster]{};
  int16_t baseW = 0;
  for (uint8_t i = 0; i < nBase; ++i) {
    advs[i] = charAdvance(atom.tcy[i], false, F);
    baseW = static_cast<int16_t>(baseW + advs[i]);
  }
  int16_t gaps[kMaxCluster]{};
  int16_t inner = 0;
  for (uint8_t i = 1; i < nBase; ++i) {
    gaps[i] = kinsokuBracketPairGap(atom.tcy[i - 1], atom.tcy[i], F);
    inner = static_cast<int16_t>(inner + gaps[i]);
  }
  const int16_t rubyUnit = static_cast<int16_t>(F / 2);
  const int16_t rubyW = static_cast<int16_t>(nRuby * (rubyUnit > 0 ? rubyUnit : 1));
  const int16_t cluster = baseW > rubyW ? baseW : rubyW;
  const int16_t span = static_cast<int16_t>(cluster + inner);
  const int16_t lead = static_cast<int16_t>((cluster - baseW) / 2);

  if (vert) {
    if (columnClosed) {
      newColumn();
    }
    if (static_cast<int>(y) + span > colBottom && y > indent) {
      const bool cannotEnd = currentCount > 0 && !kinsokuCanEndColumn(current[currentCount - 1].cp);
      const uint16_t tail = stickyTail(true);
      if (cannotEnd && tail > 0) {
        relocateTail(tail, true);
      } else {
        newColumn();
      }
      if (static_cast<int>(y) + span > colBottom && y > indent) {
        newColumn();
      }
    }
  } else if (columnClosed) {
    newLine();
  }
  if (!vert && static_cast<int>(x) + span > lineRight && x > indent) {
    const bool cannotEnd = currentCount > 0 && !kinsokuCanEndColumn(current[currentCount - 1].cp);
    const uint16_t tail = stickyTail(false);
    if (cannotEnd && tail > 0) {
      relocateTail(tail, false);
    } else {
      newLine();
    }
    if (static_cast<int>(x) + span > lineRight && x > indent) {
      newLine();
    }
  }

  int16_t leadGap = bracketGap(atom.tcy[0], F);
  const int16_t limit = vert ? colBottom : lineRight;
  const int16_t cursor = vert ? y : x;
  if (leadGap > 0 && static_cast<int>(cursor) + leadGap + span > limit && cursor > indent) {
    if (vert) {
      newColumn();
    } else {
      newLine();
    }
    leadGap = 0;
  }
  const int16_t top = vert ? y : x;
  int16_t at = static_cast<int16_t>(top + lead + leadGap);
  for (uint8_t i = 0; i < nBase; ++i) {
    at = static_cast<int16_t>(at + gaps[i]);
    GlyphRun run{};
    run.cp = atom.tcy[i];
    run.size = static_cast<uint16_t>(F);
    run.advance = static_cast<uint16_t>(advs[i] > 0 ? advs[i] : 0);
    run.emphasis = mark;
    run.box = atom.box;
    run.shade = atom.shade;
    run.rulePx = atom.rulePx;
    run.ruleEm = atom.ruleEm;
    run.ruleRoot = atom.ruleRoot;
    run.ruleSides = atom.ruleSides;
    if (bold) {
      run.flags = static_cast<uint8_t>(run.flags | kRunBold);
    }
    if (atom.family == kFaceSans) {
      run.flags = static_cast<uint8_t>(run.flags | kRunGothic);
    }
    if (i == 0) {
      copyRuby(run, atom.ruby, nRuby);
      run.rubyLead = static_cast<uint8_t>(lead > 255 ? 255 : lead);
    } else {
      run.flags = static_cast<uint8_t>(run.flags | kRunSticky);
    }
    if (advs[i] < F && kinsokuHalfAdvance(atom.tcy[i])) {
      run.flags = static_cast<uint8_t>(run.flags | kRunHalfCell);
    }
    if (!vert) {
      run.flags = static_cast<uint8_t>(run.flags | kRunRubyAbove);
    } else if (shouldRotate(run.cp)) {
      run.flags = static_cast<uint8_t>(run.flags | kRunRotate);
    }
    if (vert) {
      run.x = colX;
      run.y = at;
    } else {
      run.x = at;
      run.y = glyphY();
    }
    emit(run, feedPos);
    at = static_cast<int16_t>(at + advs[i]);
  }
  if (vert) {
    y = static_cast<int16_t>(top + span + leadGap);
  } else {
    x = static_cast<int16_t>(top + span + leadGap);
  }
}

bool PageLayouter::feed(const Atom& atom, const uint32_t atomPos) {
  pageReady = false;
  feedPos = atomPos;
  takenId = 0;
  deferredId = 0;
  switch (atom.kind) {
    case AtomKind::Picture: {
      const uint16_t id = static_cast<uint16_t>(atom.cp > 0xFFFFu ? 0xFFFFu : atom.cp);
      if (id == 0) {
        break;
      }
      const bool havePx = atom.tcyCount >= 2 && atom.tcy[0] > 0 && atom.tcy[1] > 0;
      const int pxW = havePx ? static_cast<int>(atom.tcy[0]) : 0;
      const int pxH = havePx ? static_cast<int>(atom.tcy[1]) : 0;
      /* startEm is set in memory for a cover or spine image. Pixel size is
       * tcy[0] by tcy[1]. Neither is stored in the atom file. */
      if (atom.startEm == 0 && staysInColumn(pxW, pxH, opt.width, opt.height)) {
        placePicture(id, pxW, pxH);
        break;
      }
      // One committed buffer. Glyphs already on this page stay a text page;
      // the picture is replayed from this atom on the following page.
      if (currentCount > 0) {
        newPage();
        deferredId = id;
      } else {
        newPage();
        takenId = id;
      }
      break;
    }
    case AtomKind::PageBreak:
      if (currentCount > 0) {
        newPage();
      }
      resetCursor();
      if (opt.mode == WritingMode::VerticalRl) {
        indent = colTop;
        y = colTop;
      } else {
        indent = hanmenLeft;
        x = indent;
        lineY = firstLineY;
      }
      break;
    case AtomKind::Mode: {
      const WritingMode next = atom.cp == 1 ? WritingMode::HorizontalTb : WritingMode::VerticalRl;
      if (next != opt.mode) {
        if (currentCount > 0) {
          newPage();
        }
        opt.mode = next;
        if (recomputeGrid()) {
          resetCursor();
        }
      }
      break;
    }
    case AtomKind::ColumnBreak: {
      const int extra =
          static_cast<int>(opt.em) * (static_cast<int>(atom.gapBefore) + static_cast<int>(atom.gapAfter)) / 100;
      if (opt.mode == WritingMode::VerticalRl) {
        const int16_t next = std::min(static_cast<int16_t>(colTop + atom.startEm * opt.em),
                                      static_cast<int16_t>(colBottom - opt.em));
        if (y > indent) {
          indent = next;
          newColumn(extra);
        } else {
          indent = next;
          y = indent;
          if (extra > 0 && colX != firstColX) {
            colX = static_cast<int16_t>(colX - extra);
            if (colX < hanmenLeft) {
              newPage();
            }
          }
        }
      } else {
        const int16_t next = std::min(static_cast<int16_t>(hanmenLeft + atom.startEm * opt.em),
                                      static_cast<int16_t>(lineRight - opt.em));
        if (x > indent) {
          indent = next;
          newLine(extra);
        } else {
          indent = next;
          x = indent;
          if (extra > 0 && lineY != firstLineY) {
            lineY = static_cast<int16_t>(lineY + extra);
            if (lineY + pitch > hanmenBottom) {
              newPage();
            }
          }
        }
      }
      break;
    }
    case AtomKind::Space: {
      const int16_t w = static_cast<int16_t>((opt.em * 35) / 100);
      if (opt.mode == WritingMode::VerticalRl) {
        if (columnClosed) {
          newColumn();
        }
        if (y == indent) {
          break;
        }
        if (static_cast<int>(y) + w + opt.em > colBottom) {
          newColumn();
        } else {
          y = static_cast<int16_t>(y + w);
        }
      } else {
        if (columnClosed) {
          newLine();
        }
        if (x <= indent) {
          break;
        }
        if (static_cast<int>(x) + w + opt.em > lineRight) {
          newLine();
        } else {
          x = static_cast<int16_t>(x + w);
        }
      }
      break;
    }
    case AtomKind::Gap:
      // 縦書き: 字下げ is already a fullwidth space in the text.
      if (opt.mode == WritingMode::HorizontalTb && x > indent) {
        newLine();
      }
      break;
    case AtomKind::Tcy:
      if (opt.mode == WritingMode::HorizontalTb) {
        // 横書き: an unbreakable upright run at the face advance, not a squeezed em.
        const int16_t F = facePx(opt, atom.startEm);
        const bool bold = (atom.emphasis & kEmphasisBold) != 0;
        const uint8_t mark = static_cast<uint8_t>(atom.emphasis & kEmphasisMask);
        padBox(atom.box, F);
        const uint8_t n = atom.tcyCount > kMaxCluster ? kMaxCluster : (atom.tcyCount > 0 ? atom.tcyCount : 1);
        uint32_t cps[kMaxCluster]{};
        int16_t widths[kMaxCluster]{};
        int need = 0;
        for (uint8_t i = 0; i < n; ++i) {
          cps[i] = atom.tcyCount > 0 ? atom.tcy[i] : atom.cp;
          widths[i] = charAdvance(cps[i], false, F);
          if (widths[i] < 1) {
            widths[i] = F;
          }
          need += widths[i];
        }
        if (columnClosed) {
          newLine();
        }
        if (static_cast<int>(x) + need > lineRight && x > indent) {
          newLine();
        }
        for (uint8_t i = 0; i < n; ++i) {
          GlyphRun run{};
          run.cp = cps[i];
          run.size = static_cast<uint16_t>(F);
          run.advance = static_cast<uint16_t>(widths[i]);
          run.emphasis = mark;
          run.box = atom.box;
          run.shade = atom.shade;
          run.rulePx = atom.rulePx;
          run.ruleEm = atom.ruleEm;
          run.ruleRoot = atom.ruleRoot;
          run.ruleSides = atom.ruleSides;
          run.x = x;
          run.y = glyphY();
          run.flags = kRunRubyAbove;
          if (bold) {
            run.flags = static_cast<uint8_t>(run.flags | kRunBold);
          }
          if (atom.family == kFaceSans) {
            run.flags = static_cast<uint8_t>(run.flags | kRunGothic);
          }
          if (i > 0) {
            run.flags = static_cast<uint8_t>(run.flags | kRunSticky);
          }
          emit(run, feedPos);
          x = static_cast<int16_t>(x + widths[i]);
        }
      } else {
        const uint8_t n = atom.tcyCount > kMaxCluster ? kMaxCluster : (atom.tcyCount > 0 ? atom.tcyCount : 1);
        const uint32_t* cps = atom.tcyCount > 0 ? atom.tcy : &atom.cp;
        placeChar(atom.cp, nullptr, 0, true, static_cast<uint8_t>(atom.emphasis & kEmphasisMask), cps, n, atom.startEm,
                  (atom.emphasis & kEmphasisBold) != 0, atom.box, atom.shade, atom.rulePx, atom.ruleEm, atom.ruleRoot,
                  atom.ruleSides, atom.family == kFaceSans);
      }
      lastAtomPos = atomPos;
      break;
    case AtomKind::Group:
      placeGroup(atom);
      lastAtomPos = atomPos;
      break;
    case AtomKind::Ch:
      placeChar(atom.cp, atom.ruby, atom.rubyCount, false, static_cast<uint8_t>(atom.emphasis & kEmphasisMask), nullptr,
                0, atom.startEm, (atom.emphasis & kEmphasisBold) != 0, atom.box, atom.shade, atom.rulePx, atom.ruleEm,
                atom.ruleRoot, atom.ruleSides, atom.family == kFaceSans);
      lastAtomPos = atomPos;
      break;
  }
  return pageReady;
}

bool PageLayouter::finish() {
  if (currentCount == 0) {
    return false;
  }
  newPage();
  return true;
}

RubyAlong placeRubyAlong(const int16_t origin, const int16_t em, const int16_t rubyEm, const uint8_t count,
                         const bool loEmpty, const bool hiEmpty) {
  RubyAlong out{};
  if (count == 0 || em <= 0 || rubyEm <= 0) {
    return out;
  }
  int lo = origin;
  int hi = origin + em;
  if (loEmpty) {
    lo -= em;
  }
  if (hiEmpty) {
    hi += em;
  }
  int pitch = rubyEm;
  int span = static_cast<int>(count) * pitch;
  const int room = hi - lo;
  if (span > room) {
    pitch = room / count;
    if (pitch < 1) {
      pitch = 1;
    }
    span = static_cast<int>(count) * pitch;
  }
  int start = origin + (em - span) / 2;
  if (start < lo) {
    start = lo;
  }
  if (start + span > hi) {
    start = hi - span;
  }
  out.start = static_cast<int16_t>(start);
  out.pitch = static_cast<int16_t>(pitch);
  return out;
}

}  // namespace ts
