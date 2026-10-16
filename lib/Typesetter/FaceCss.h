#pragma once

#include <cstddef>
#include <cstdint>

namespace ts {

// One simple selector from a stylesheet: `strong`, `.bold`, or `span.big`.
// weight 0 leaves the parent, 1 is normal, 2 is bold.
// pct 0 leaves the parent. Otherwise it is a percent of the parent em, or of
// the body em when root is set (rem).
constexpr uint8_t kMaxFaceRules = 96;

struct FaceRule {
  char tag[12]{};
  char cls[32]{};
  uint8_t weight = 0;
  uint8_t pct = 0;
  uint8_t root = 0;
};

// Pull font-weight and font-size out of a declaration list. Leaves a field
// alone when that property is absent.
void readFaceDecls(const char* s, size_t n, uint8_t& weight, uint8_t& pct, uint8_t& root);

// Replace /* */ comments with spaces. `s` is mutable CSS text.
void blankCssComments(char* s, size_t n);

// Append rules from one stylesheet. Selectors are an element, a class, or both.
void scanFaceCss(char* css, size_t n, FaceRule* rules, uint8_t& count);

// Append rules from <style> blocks in the chapter. Linked sheets are separate.
void scanStyleTags(const char* html, size_t n, FaceRule* rules, uint8_t& count);

}  // namespace ts
