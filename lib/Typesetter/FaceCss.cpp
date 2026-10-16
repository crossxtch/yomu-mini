#include "FaceCss.h"

#include <cstdlib>
#include <cstring>

namespace ts {


static bool faceSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static bool faceKey(const char* s, const size_t n, const size_t i, const char* key, size_t& valueAt) {
  const size_t kn = strlen(key);
  if (i + kn >= n) {
    return false;
  }
  for (size_t k = 0; k < kn; ++k) {
    char c = s[i + k];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != key[k]) {
      return false;
    }
  }
  if (i > 0) {
    const unsigned char prev = static_cast<unsigned char>(s[i - 1]);
    if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9') || prev == '_') {
      return false;
    }
  }
  size_t j = i + kn;
  while (j < n && faceSpace(s[j])) {
    ++j;
  }
  if (j >= n || s[j] != ':') {
    return false;
  }
  ++j;
  while (j < n && faceSpace(s[j])) {
    ++j;
  }
  valueAt = j;
  return true;
}

static bool startsWord(const char* s, const size_t n, const char* word) {
  const size_t wn = strlen(word);
  if (n < wn) {
    return false;
  }
  for (size_t i = 0; i < wn; ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != word[i]) {
      return false;
    }
  }
  if (n == wn) {
    return true;
  }
  const char c = s[wn];
  return faceSpace(c) || c == ';' || c == '}' || c == '!' || c == ',';
}

static uint8_t clampPct(const int v) {
  if (v < 40) {
    return 40;
  }
  if (v > 250) {
    return 250;
  }
  return static_cast<uint8_t>(v);
}

static bool parseWeight(const char* s, const size_t n, uint8_t& weight) {
  size_t i = 0;
  while (i < n && faceSpace(s[i])) {
    ++i;
  }
  if (i >= n) {
    return false;
  }
  const char* p = s + i;
  const size_t left = n - i;
  if (startsWord(p, left, "inherit")) {
    return false;
  }
  if (startsWord(p, left, "bold") || startsWord(p, left, "bolder")) {
    weight = 2;
    return true;
  }
  if (startsWord(p, left, "normal") || startsWord(p, left, "lighter")) {
    weight = 1;
    return true;
  }
  if (p[0] < '0' || p[0] > '9') {
    return false;
  }
  int v = 0;
  for (size_t k = 0; k < left && p[k] >= '0' && p[k] <= '9'; ++k) {
    v = v * 10 + (p[k] - '0');
  }
  weight = v >= 600 ? 2 : 1;
  return true;
}

static bool parseSize(const char* s, const size_t n, uint8_t& pct, uint8_t& root) {
  size_t i = 0;
  while (i < n && faceSpace(s[i])) {
    ++i;
  }
  if (i >= n) {
    return false;
  }
  const char* p = s + i;
  const size_t left = n - i;
  struct Kw {
    const char* word;
    uint8_t pct;
  };
  const Kw kws[] = {{"xx-small", 58}, {"x-small", 69}, {"smaller", 83}, {"small", 83}, {"medium", 100},
                    {"larger", 120},   {"x-large", 144}, {"xx-large", 200}, {"large", 120}};
  for (const Kw& kw : kws) {
    if (startsWord(p, left, kw.word)) {
      pct = kw.pct;
      root = 0;
      return true;
    }
  }
  if (p[0] < '0' || p[0] > '9') {
    return false;
  }
  int whole = 0;
  size_t k = 0;
  while (k < left && p[k] >= '0' && p[k] <= '9') {
    whole = whole * 10 + (p[k] - '0');
    if (whole > 1000) {
      return false;
    }
    ++k;
  }
  int frac = 0;
  int digits = 0;
  if (k < left && p[k] == '.') {
    ++k;
    while (k < left && p[k] >= '0' && p[k] <= '9') {
      if (digits < 4) {
        frac = frac * 10 + (p[k] - '0');
        ++digits;
      }
      ++k;
    }
  }
  while (k < left && faceSpace(p[k])) {
    ++k;
  }
  if (k >= left) {
    return false;
  }
  int div = 1;
  for (int d = 0; d < digits; ++d) {
    div *= 10;
  }
  const bool rem = k + 2 < left && (p[k] == 'r' || p[k] == 'R') && (p[k + 1] == 'e' || p[k + 1] == 'E') &&
                   (p[k + 2] == 'm' || p[k + 2] == 'M');
  const bool em = !rem && k + 1 < left && (p[k] == 'e' || p[k] == 'E') && (p[k + 1] == 'm' || p[k + 1] == 'M');
  const bool percent = p[k] == '%';
  if (!rem && !em && !percent) {
    return false;
  }
  int v = percent ? whole : whole * 100;
  if (digits > 0) {
    const int scale = percent ? 1 : 100;
    v += (frac * scale + div / 2) / div;
  }
  if (v <= 0) {
    return false;
  }
  pct = clampPct(v);
  root = rem ? 1 : 0;
  return true;
}

static size_t declEnd(const char* s, const size_t n, const size_t i) {
  size_t j = i;
  while (j < n && s[j] != ';' && s[j] != '}' && s[j] != '!') {
    ++j;
  }
  return j;
}


static bool cssSpace(const char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

constexpr uint32_t kMaxCssBytes = 65536;

void blankCssComments(char* s, const size_t n) {
  for (size_t i = 0; i + 1 < n; ++i) {
    if (s[i] != '/' || s[i + 1] != '*') {
      continue;
    }
    size_t j = i + 2;
    while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) {
      ++j;
    }
    if (j + 1 < n) {
      j += 2;
    } else {
      j = n;
    }
    for (size_t k = i; k < j; ++k) {
      s[k] = ' ';
    }
    i = j > 0 ? j - 1 : 0;
  }
}

static bool faceChar(const unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void addFaceSelector(const char* s, size_t n, const uint8_t weight, const uint8_t pct, const uint8_t root,
                     FaceRule* rules, uint8_t& count) {
  if (!rules || count >= kMaxFaceRules || (weight == 0 && pct == 0)) {
    return;
  }
  while (n > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) {
    ++s;
    --n;
  }
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) {
    --n;
  }
  if (n == 0) {
    return;
  }
  int dots = 0;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '.') {
      ++dots;
      continue;
    }
    if (!faceChar(c)) {
      return;
    }
  }
  if (dots > 1) {
    return;
  }
  FaceRule rule{};
  rule.weight = weight;
  rule.pct = pct;
  rule.root = root;
  const char* dot = nullptr;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == '.') {
      dot = s + i;
      break;
    }
  }
  if (dot) {
    const size_t tagN = static_cast<size_t>(dot - s);
    const size_t clsN = n - tagN - 1;
    if (tagN >= sizeof(rule.tag) || clsN == 0 || clsN >= sizeof(rule.cls)) {
      return;
    }
    for (size_t i = 0; i < tagN; ++i) {
      char c = s[i];
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
      rule.tag[i] = c;
    }
    memcpy(rule.cls, dot + 1, clsN);
  } else if (s[0] == '.') {
    return;
  } else if (n >= sizeof(rule.tag)) {
    return;
  } else {
    for (size_t i = 0; i < n; ++i) {
      char c = s[i];
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
      rule.tag[i] = c;
    }
  }
  rules[count++] = rule;
}

static void addFaceSelectors(const char* sel, const size_t n, const uint8_t weight, const uint8_t pct, const uint8_t root,
                      FaceRule* rules, uint8_t& count) {
  size_t start = 0;
  for (size_t i = 0; i <= n && count < kMaxFaceRules; ++i) {
    if (i < n && sel[i] != ',') {
      continue;
    }
    addFaceSelector(sel + start, i - start, weight, pct, root, rules, count);
    start = i + 1;
  }
}

static bool hasFaceKey(const char* css, const size_t n, const size_t i, const char* key) {
  const size_t kn = strlen(key);
  if (i + kn >= n) {
    return false;
  }
  for (size_t k = 0; k < kn; ++k) {
    char c = css[i + k];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != key[k]) {
      return false;
    }
  }
  if (i > 0) {
    const unsigned char prev = static_cast<unsigned char>(css[i - 1]);
    if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9') || prev == '_') {
      return false;
    }
  }
  size_t j = i + kn;
  while (j < n && cssSpace(css[j])) {
    ++j;
  }
  return j < n && css[j] == ':';
}

void scanFaceCss(char* css, const size_t n, FaceRule* rules, uint8_t& count) {
  if (!css || n == 0 || !rules || count >= kMaxFaceRules) {
    return;
  }
  blankCssComments(css, n);
  for (size_t i = 0; i + 10 < n && count < kMaxFaceRules; ++i) {
    if (!hasFaceKey(css, n, i, "font-weight") && !hasFaceKey(css, n, i, "font-size")) {
      continue;
    }
    size_t brace = i;
    while (brace > 0 && css[brace] != '{') {
      --brace;
    }
    if (css[brace] != '{') {
      continue;
    }
    size_t selBeg = brace;
    while (selBeg > 0 && css[selBeg - 1] != '}' && css[selBeg - 1] != '{') {
      --selBeg;
    }
    size_t end = brace + 1;
    while (end < n && css[end] != '}') {
      ++end;
    }
    uint8_t weight = 0;
    uint8_t pct = 0;
    uint8_t root = 0;
    readFaceDecls(css + brace + 1, end > brace ? end - (brace + 1) : 0, weight, pct, root);
    addFaceSelectors(css + selBeg, brace - selBeg, weight, pct, root, rules, count);
    if (end < n) {
      i = end;
    }
  }
}

void scanStyleTags(const char* html, const size_t n, FaceRule* rules, uint8_t& count) {
  if (!html || n < 14 || !rules) {
    return;
  }
  for (size_t i = 0; i + 8 < n && count < kMaxFaceRules; ++i) {
    if (html[i] != '<') {
      continue;
    }
    const char s0 = static_cast<char>(html[i + 1] | 0x20);
    const char s1 = static_cast<char>(html[i + 2] | 0x20);
    const char s2 = static_cast<char>(html[i + 3] | 0x20);
    const char s3 = static_cast<char>(html[i + 4] | 0x20);
    const char s4 = static_cast<char>(html[i + 5] | 0x20);
    if (s0 != 's' || s1 != 't' || s2 != 'y' || s3 != 'l' || s4 != 'e') {
      continue;
    }
    const unsigned char edge = static_cast<unsigned char>(html[i + 6]);
    if (faceChar(edge)) {
      continue;
    }
    size_t body = i + 6;
    while (body < n && html[body] != '>') {
      ++body;
    }
    if (body >= n) {
      break;
    }
    ++body;
    size_t close = body;
    while (close + 8 <= n) {
      if (html[close] == '<' && html[close + 1] == '/' && (html[close + 2] | 0x20) == 's' &&
          (html[close + 3] | 0x20) == 't' && (html[close + 4] | 0x20) == 'y' && (html[close + 5] | 0x20) == 'l' &&
          (html[close + 6] | 0x20) == 'e') {
        break;
      }
      ++close;
    }
    if (close + 8 > n || close <= body) {
      i = close;
      continue;
    }
    const size_t len = close - body;
    if (len > kMaxCssBytes) {
      i = close;
      continue;
    }
    char* css = static_cast<char*>(malloc(len));
    if (css) {
      memcpy(css, html + body, len);
      scanFaceCss(css, len, rules, count);
      free(css);
    }
    i = close;
  }
}

void readFaceDecls(const char* s, const size_t n, uint8_t& weight, uint8_t& pct, uint8_t& root) {
  if (!s || n == 0) {
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    size_t at = 0;
    if (faceKey(s, n, i, "font-weight", at)) {
      const size_t end = declEnd(s, n, at);
      uint8_t w = 0;
      if (parseWeight(s + at, end - at, w)) {
        weight = w;
      }
      i = end > i ? end - 1 : i;
      continue;
    }
    if (faceKey(s, n, i, "font-size", at)) {
      const size_t end = declEnd(s, n, at);
      uint8_t p = 0;
      uint8_t r = 0;
      if (parseSize(s + at, end - at, p, r)) {
        pct = p;
        root = r;
      }
      i = end > i ? end - 1 : i;
    }
  }
}

}  // namespace ts
