#pragma once

#include <cstddef>
#include <cstdint>

namespace ts {

// One simple selector from a stylesheet: `strong`, `.bold`, or `span.big`.
// weight 0 leaves the parent, 1 is normal, 2 is bold.
// pct 0 leaves the parent. Otherwise it is a percent of the parent em, or of
// the body em when root is set (rem).
// emphasis 0 leaves the parent. kFaceEmNone clears it. Any other value is a kEm* mark.
// box 0 leaves the parent, 1 is no rule, 2 starts a 囲み罫 when the parent has none.
// shade 0 leaves the parent, 1 is no fill, and 2–255 is an 8-bit gray (black is 2).
// rulePx 0 leaves the em thickness. Otherwise it is the CSS border width in px.
// ruleEm is a percent of the em for an em or rem width. ruleRoot 1 means rem.
// ruleSides marks a two- or three-side rule. 0 draws every side of a box.
// A long advance sheet has about a thousand simple rules, so the cap is past 96.
constexpr uint16_t kMaxFaceRules = 1536;
constexpr uint8_t kFaceEmNone = 0xFFu;
constexpr uint8_t kFaceSerif = 1;
constexpr uint8_t kFaceSans = 2;

struct FaceRule {
  char tag[12]{};
  char cls[32]{};
  uint8_t weight = 0;
  uint8_t pct = 0;
  uint8_t root = 0;
  uint8_t emphasis = 0;
  uint8_t box = 0;
  uint8_t shade = 0;
  uint8_t rulePx = 0;
  uint8_t ruleEm = 0;
  uint8_t ruleRoot = 0;
  uint8_t ruleSides = 0;
  uint8_t family = 0;  // 0 inherits. kFaceSerif or kFaceSans.
  uint8_t mode = 0;    // 0 any chapter. 1 horizontal. 2 vertical.
};

// One declaration list, reduced to the allowlist. A zero field was absent.
struct FacePaint {
  uint8_t weight = 0;
  uint8_t pct = 0;
  uint8_t root = 0;
  uint8_t emphasis = 0;
  uint8_t box = 0;
  uint8_t shade = 0;
  uint8_t rulePx = 0;
  uint8_t ruleEm = 0;
  uint8_t ruleRoot = 0;
  uint8_t ruleSides = 0;
  uint8_t family = 0;  // 0 was absent. kFaceSerif or kFaceSans.
};

// Pull weight, size, text-emphasis, border, and background out of a declaration list.
void readFaceDecls(const char* s, size_t n, FacePaint& out);

// Replace /* */ comments with spaces. `s` is mutable CSS text.
void blankCssComments(char* s, size_t n);

// Append rules from one stylesheet. Selectors are an element, a class, or both.
void scanFaceCss(char* css, size_t n, FaceRule* rules, uint16_t& count);

// Append rules from <style> blocks in the chapter. Linked sheets are separate.
void scanStyleTags(const char* html, size_t n, FaceRule* rules, uint16_t& count);

// Append @import targets. Comments in `css` are blanked. `count` is the next free slot.
int appendCssImports(char* css, size_t n, char out[][96], int count, int cap);

}  // namespace ts
