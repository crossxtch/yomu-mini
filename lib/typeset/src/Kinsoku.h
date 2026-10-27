#pragma once

#include <cstdint>

namespace ts {

bool kinsokuCanStartColumn(uint32_t cp);
bool kinsokuCanEndColumn(uint32_t cp);
bool shouldRotate(uint32_t cp);

// 。、．， may sit past the 版面. Brackets do not.
bool kinsokuHangs(uint32_t cp);
// Brackets, ・, ：, and hanging 句読点. Half an em. The vertical glyph is
// drawn on the full em and slid into this shorter cell.
bool kinsokuHalfAdvance(uint32_t cp);
// Extra space before `next` when `prev` is a closing bracket and `next` is an
// opening bracket. Each is already half an em, so the strokes would meet.
int16_t kinsokuBracketPairGap(uint32_t prev, uint32_t next, int16_t em);
// Punctuation whose ink sits in a corner of the em. Center it in the cell.
bool kinsokuYakumono(uint32_t cp);
// CJK, kana, and fullwidth stay on the em grid. ASCII and Latin-1 use the
// face advance in 横書き.
bool usesEmAdvance(uint32_t cp);
int16_t kinsokuInlineAdvance(uint32_t cp, int16_t em, bool tcy);

inline bool isHiragana(const uint32_t cp) { return cp >= 0x3040 && cp <= 0x309F; }
inline bool isKatakana(const uint32_t cp) { return cp >= 0x30A0 && cp <= 0x30FF; }
inline bool isKanji(const uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

}  // namespace ts
