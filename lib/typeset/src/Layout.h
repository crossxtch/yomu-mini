#pragma once

#include <cstdint>

namespace ts {

enum class AtomKind : uint8_t {
  Ch,
  Tcy,
  Gap,
  Space,
  ColumnBreak,
  PageBreak,
  Group,    // unbreakable 熟語ルビ: tcy[] = bases, ruby[] = reading
  Mode,     // cp 0 = vertical-rl, 1 = horizontal-tb
  Picture,  // cp = 1-based page id in the picture sidecar. 0 is not a picture.
};

// Picture atom cp is the sidecar id in the low 15 bits. The high bit forces a
// page of its own (a cover, 扉, or a spine image). Pixel size is not stored.
constexpr uint16_t kPictureForcePage = 0x8000u;
constexpr uint16_t kPictureIdMask = 0x7fffu;

// Readings, 縦中横, and 熟語 bases kept on one atom or glyph.
constexpr uint8_t kMaxCluster = 8;

struct Atom {
  AtomKind kind = AtomKind::Ch;
  uint32_t cp = 0;  // Ch: base. Tcy/Group: first base. Mode: 0 vert, 1 horiz.
                    // ColumnBreak, in memory only: 1 starts a heading. Not stored.
  uint8_t rubyCount = 0;
  uint32_t ruby[kMaxCluster]{};  // Ch/Group reading. Tcy glyphs reuse this only on the run.
  uint8_t tcyCount = 0;
  uint32_t tcy[kMaxCluster]{};  // Tcy chars, or Group bases (base 0 duplicated in cp)
  uint8_t startEm = 0;  // ColumnBreak indent. Ch/Tcy/Group: percent of the body em, 0 means 100.
  uint8_t emphasis = 0;  // low bits: a kEm* mark. kEmphasisBold is a heavier face.
  uint8_t box = 0;       // 囲み罫 id. 0 is none. One id is one rule per column or line.
  uint8_t shade = 0;     // 0 is no fill. 2–255 is the background gray; black is stored as 2.
  uint8_t rulePx = 0;    // 0 keeps the em thickness. Otherwise the CSS border width in px.
  uint8_t ruleEm = 0;    // Percent of the em when the width is em or rem. 0 means absent.
  uint8_t ruleRoot = 0;  // 1 uses the body em (rem). 0 uses this glyph's em.
  uint8_t ruleSides = 0; // Partial 囲み罫. 0 draws every side. See kBorder*.
  uint8_t family = 0;    // 0 is the body face. kFaceSans draws gothic.
  uint8_t gapBefore = 0; // ColumnBreak: percent of the em before this column.
  uint8_t gapAfter = 0;  // ColumnBreak: percent of the em after this column.
};

constexpr uint8_t kEmphasisMask = 0x7f;
constexpr uint8_t kEmphasisBold = 0x80;
// Low 7 bits. 1 and 2 stay the old 圏点 and 傍線 codes.
constexpr uint8_t kEmSesame = 1;
constexpr uint8_t kEmLine = 2;
constexpr uint8_t kEmSesameOpen = 3;
constexpr uint8_t kEmDot = 4;
constexpr uint8_t kEmDotOpen = 5;
constexpr uint8_t kEmCircle = 6;
constexpr uint8_t kEmCircleOpen = 7;
constexpr uint8_t kEmDoubleCircle = 8;
constexpr uint8_t kEmDoubleCircleOpen = 9;
constexpr uint8_t kEmTriangle = 10;

// flags: bit0 rotate90, bit1 tcy, bit2 rubyAbove, bit3 center ink in `advance`, bit4 sticky
constexpr uint8_t kRunRotate = 1;
constexpr uint8_t kRunTcy = 2;
constexpr uint8_t kRunRubyAbove = 4;
constexpr uint8_t kRunHalfCell = 8;
constexpr uint8_t kRunSticky = 16;
constexpr uint8_t kRunPicture = 32;  // cp is a picture id; size is the box width, advance the height
constexpr uint8_t kRunBold = 64;
constexpr uint8_t kRunGothic = 128;

struct GlyphRun {
  uint32_t cp = 0;
  uint32_t ruby[kMaxCluster]{};  // reading, or the tcy chars when kRunTcy is set
  int16_t x = 0;
  int16_t y = 0;
  uint16_t size = 0;
  uint16_t advance = 0;  // inline px; 0 means `size`
  uint8_t rubyCount = 0;
  uint8_t flags = 0;
  uint8_t emphasis = 0;  // 0 none, otherwise a kEm* mark
  uint8_t rubyLead = 0;  // px before this glyph still inside the ruby span
  uint8_t box = 0;       // 囲み罫 id. 0 is none.
  uint8_t shade = 0;     // 0 is no fill. Otherwise the background gray.
  uint8_t rulePx = 0;    // 0 keeps the em thickness. Otherwise the CSS border width in px.
  uint8_t ruleEm = 0;    // Percent of the em when the width is em or rem. 0 means absent.
  uint8_t ruleRoot = 0;  // 1 uses the body em (rem). 0 uses this glyph's em.
  uint8_t ruleSides = 0; // Partial 囲み罫. 0 draws every side. See kBorder*.
};

// Bits of a partial 囲み罫. Top, right, bottom, left, in that CSS order.
constexpr uint8_t kBorderTop = 1;
constexpr uint8_t kBorderRight = 2;
constexpr uint8_t kBorderBottom = 4;
constexpr uint8_t kBorderLeft = 8;

inline bool runRotate90(const GlyphRun& g) { return (g.flags & kRunRotate) != 0; }
inline bool runTcy(const GlyphRun& g) { return (g.flags & kRunTcy) != 0; }
inline bool runRubyAbove(const GlyphRun& g) { return (g.flags & kRunRubyAbove) != 0; }
inline bool runSticky(const GlyphRun& g) { return (g.flags & kRunSticky) != 0; }
inline bool runPicture(const GlyphRun& g) { return (g.flags & kRunPicture) != 0; }
inline int16_t runAdvance(const GlyphRun& g) {
  return g.advance ? static_cast<int16_t>(g.advance) : static_cast<int16_t>(g.size);
}

// One page of runs, plus the layouter's current and committed pages.
static_assert(sizeof(GlyphRun) <= 64, "GlyphRun grew past the page budget");

// Place ruby along the base cell [origin, origin+em). Overhang into an empty
// neighbor cell (no ruby, or no adjacent glyph) is allowed; a neighboring ruby
// cluster blocks that side. If the reading still does not fit, pitch shrinks.
struct RubyAlong {
  int16_t start = 0;
  int16_t pitch = 0;
};

RubyAlong placeRubyAlong(int16_t origin, int16_t em, int16_t rubyEm, uint8_t count, bool loEmpty,
                         bool hiEmpty);

enum class WritingMode : uint8_t { VerticalRl, HorizontalTb };

struct LayoutOptions {
  int16_t width = 528;
  int16_t height = 792;
  uint8_t em = 47;
  int16_t margin = 0;  // top and bottom only, in pixels. The sides stay at the edge.
  bool compactColumns = false;
  bool hasRuby = true;
  WritingMode mode = WritingMode::VerticalRl;
  // 横書き Latin advance in px. Null keeps every non-約物 glyph on the em grid.
  int16_t (*measure)(void* ctx, uint32_t cp, int16_t em) = nullptr;
  void* measureCtx = nullptr;
};

/* Paperwhite 11 landscape at the smallest type step is 31 columns by 35 em
 * (about 1100 glyphs). Half-em punctuation and 縦中横 add more. 256 was the
 * Xteink page, so that panel committed after the right-hand columns and
 * left the rest of the width blank. The firmware build passes
 * -DTS_MAX_GLYPHS=256. */
#ifndef TS_MAX_GLYPHS
#define TS_MAX_GLYPHS 2048
#endif

class PageLayouter {
 public:
  static constexpr uint16_t kMaxGlyphs = TS_MAX_GLYPHS;

  void begin(const LayoutOptions& opt);
  // `atomPos` is the source byte offset of this atom (for page-index building).
  // Returns true when a full page was committed into `page()`. The current
  // atom may already sit on the next page (kinsoku / overflow).
  bool feed(const Atom& atom, uint32_t atomPos = 0);
  // Commit leftover glyphs as a last page. Returns true if anything was left.
  bool finish();

  const GlyphRun* page() const { return committed; }
  uint16_t pageGlyphCount() const { return committedCount; }
  void clearPage() { committedCount = 0; }
  // Byte offset of the first atom on the in-progress page (valid after feed
  // returned true and placed overflow/kinsoku glyphs on the new page).
  uint32_t currentPagePos() const { return firstOnCurrent; }
  uint16_t currentGlyphCount() const { return currentCount; }
  // This feed committed an empty page whose bitmap is `cp`. The page is already
  // the in-progress one (or must be pushed when the index was waiting).
  uint16_t takenPicture() const { return takenId; }
  // Glyphs were committed, and this picture atom has to be replayed as the next page.
  uint16_t deferredPicture() const { return deferredId; }

 private:
  LayoutOptions opt{};
  int16_t pitch = 0;
  int16_t rubyGutter = 0;
  int16_t hanmenLeft = 0;
  int16_t firstColX = 0;
  int16_t colTop = 0;
  int16_t colBottom = 0;
  int16_t lineRight = 0;
  int16_t firstLineY = 0;
  int16_t hanmenBottom = 0;
  int16_t indent = 0;  // indentY (vert) or indentX (horiz)
  int16_t colX = 0;
  int16_t y = 0;
  int16_t x = 0;
  int16_t lineY = 0;

  GlyphRun current[kMaxGlyphs]{};
  uint16_t currentCount = 0;
  GlyphRun committed[kMaxGlyphs]{};
  uint16_t committedCount = 0;
  bool pageReady = false;
  uint32_t lastAtomPos = 0;
  uint32_t firstOnCurrent = 0;
  uint32_t feedPos = 0;
  uint16_t takenId = 0;
  uint16_t deferredId = 0;

  bool columnClosed = false;
  uint8_t lastBox = 0;

  bool recomputeGrid();
  void resetCursor();
  int16_t charAdvance(uint32_t cp, bool tcy, int16_t em) const;
  // Half an em when the previous glyph on this column is a closing bracket
  // and `cp` is an opening bracket. Zero at a column or line start.
  int16_t bracketGap(uint32_t cp, int16_t em) const;
  void newPage();
  void newColumn(int extra = 0);
  void newLine(int extra = 0);
  void emit(const GlyphRun& run, uint32_t pos);
  void padBox(uint8_t box, int16_t em);
  void placeChar(uint32_t cp, const uint32_t* ruby, uint8_t rubyCount, bool tcy, uint8_t emphasis,
                 const uint32_t* tcyCps, uint8_t tcyN, uint8_t scale, uint8_t bold, uint8_t box, uint8_t shade,
                 uint8_t rulePx, uint8_t ruleEm, uint8_t ruleRoot, uint8_t ruleSides, uint8_t sans);
  void placeGroup(const Atom& atom);
  void placePicture(uint16_t id, int pxW, int pxH);
  uint16_t stickyTail(bool vertical) const;
  void relocateTail(uint16_t n, bool vertical);
  int16_t glyphY() const { return static_cast<int16_t>(lineY + rubyGutter); }
};

}  // namespace ts
