#include "FaceCss.h"

#include "Layout.h"

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

constexpr uint32_t kMaxCssBytes = 2u * 1024u * 1024u;

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

static bool sameToken(const char* s, const size_t n, const char* lit) {
  const size_t m = strlen(lit);
  if (n != m) {
    return false;
  }
  for (size_t i = 0; i < m; ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != lit[i]) {
      return false;
    }
  }
  return true;
}

// `.vrtl`, `html.vrtl`, and `.vertical-rl` keep the subject for a vertical chapter.
// The horizontal names are `.hltr`, `html.hltr`, and `.horizontal-tb`.
static bool modeGuard(const char* s, size_t n, uint8_t& mode) {
  while (n > 0 && faceSpace(s[0])) {
    ++s;
    --n;
  }
  while (n > 0 && faceSpace(s[n - 1])) {
    --n;
  }
  const char* dot = nullptr;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == '.') {
      dot = s + i;
      break;
    }
    if (!faceChar(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  if (!dot) {
    return false;
  }
  const char* cls = dot + 1;
  const size_t clsN = n - static_cast<size_t>(cls - s);
  if (sameToken(cls, clsN, "vrtl") || sameToken(cls, clsN, "vertical-rl")) {
    mode = 2;
    return true;
  }
  if (sameToken(cls, clsN, "hltr") || sameToken(cls, clsN, "horizontal-tb")) {
    mode = 1;
    return true;
  }
  return false;
}

static void addFaceSelector(const char* s, size_t n, const FacePaint& paint, FaceRule* rules, uint16_t& count) {
  if (!rules || count >= kMaxFaceRules ||
      (paint.weight == 0 && paint.pct == 0 && paint.emphasis == 0 && paint.box == 0 && paint.shade == 0 &&
       paint.rulePx == 0 && paint.ruleEm == 0 && paint.ruleSides == 0 && paint.family == 0)) {
    return;
  }
  while (n > 0 && faceSpace(s[0])) {
    ++s;
    --n;
  }
  while (n > 0 && faceSpace(s[n - 1])) {
    --n;
  }
  if (n == 0) {
    return;
  }
  uint8_t mode = 0;
  size_t split = n;
  for (size_t i = 0; i < n; ++i) {
    if (faceSpace(s[i])) {
      split = i;
    }
  }
  if (split < n) {
    if (!modeGuard(s, split, mode)) {
      return;
    }
    size_t sub = split + 1;
    while (sub < n && faceSpace(s[sub])) {
      ++sub;
    }
    if (sub >= n) {
      return;
    }
    s += sub;
    n -= sub;
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
  rule.weight = paint.weight;
  rule.pct = paint.pct;
  rule.root = paint.root;
  rule.emphasis = paint.emphasis;
  rule.box = paint.box;
  rule.shade = paint.shade;
  rule.rulePx = paint.rulePx;
  rule.ruleEm = paint.ruleEm;
  rule.ruleRoot = paint.ruleRoot;
  rule.ruleSides = paint.ruleSides;
  rule.family = paint.family;
  rule.mode = mode;
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

static void addFaceSelectors(const char* sel, const size_t n, const FacePaint& paint, FaceRule* rules,
                             uint16_t& count) {
  size_t start = 0;
  for (size_t i = 0; i <= n && count < kMaxFaceRules; ++i) {
    if (i < n && sel[i] != ',') {
      continue;
    }
    addFaceSelector(sel + start, i - start, paint, rules, count);
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

static bool hasAllowKey(const char* css, const size_t n, const size_t i) {
  static const char* keys[] = {"font-family",
                               "font-weight",
                               "font-size",
                               "text-emphasis-style",
                               "text-emphasis",
                               "-webkit-text-emphasis-style",
                               "border-top-width",
                               "border-right-width",
                               "border-bottom-width",
                               "border-left-width",
                               "border-width",
                               "border-top-style",
                               "border-right-style",
                               "border-bottom-style",
                               "border-left-style",
                               "border-style",
                               "border-top",
                               "border-right",
                               "border-bottom",
                               "border-left",
                               "border",
                               "background-color",
                               "background"};
  for (const char* key : keys) {
    if (hasFaceKey(css, n, i, key)) {
      return true;
    }
  }
  return false;
}

void scanFaceCss(char* css, const size_t n, FaceRule* rules, uint16_t& count) {
  if (!css || n == 0 || !rules || count >= kMaxFaceRules) {
    return;
  }
  blankCssComments(css, n);
  for (size_t i = 0; i + 10 < n && count < kMaxFaceRules; ++i) {
    if (!hasAllowKey(css, n, i)) {
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
    while (selBeg > 0 && css[selBeg - 1] != '}' && css[selBeg - 1] != '{' && css[selBeg - 1] != ';') {
      --selBeg;
    }
    size_t end = brace + 1;
    while (end < n && css[end] != '}') {
      ++end;
    }
    FacePaint paint{};
    readFaceDecls(css + brace + 1, end > brace ? end - (brace + 1) : 0, paint);
    addFaceSelectors(css + selBeg, brace - selBeg, paint, rules, count);
    if (end < n) {
      i = end;
    }
  }
}

void scanStyleTags(const char* html, const size_t n, FaceRule* rules, uint16_t& count) {
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

static bool atImport(const char* s, const size_t n, const size_t i) {
  static const char key[] = "@import";
  if (i + 7 > n) {
    return false;
  }
  for (size_t k = 0; k < 7; ++k) {
    char c = s[i + k];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != key[k]) {
      return false;
    }
  }
  if (i > 0 && faceChar(static_cast<unsigned char>(s[i - 1]))) {
    return false;
  }
  if (i + 7 < n && faceChar(static_cast<unsigned char>(s[i + 7]))) {
    return false;
  }
  return true;
}

static bool endsCssName(const char* s) {
  const size_t n = strlen(s);
  if (n < 4) {
    return false;
  }
  const char* e = s + n - 4;
  return (e[0] == '.' && (e[1] == 'c' || e[1] == 'C') && (e[2] == 's' || e[2] == 'S') &&
          (e[3] == 's' || e[3] == 'S'));
}

int appendCssImports(char* css, const size_t n, char out[][96], int count, const int cap) {
  if (!css || !out || cap <= 0) {
    return count > 0 ? count : 0;
  }
  if (count < 0) {
    count = 0;
  }
  if (count > cap) {
    count = cap;
  }
  blankCssComments(css, n);
  for (size_t i = 0; i + 7 <= n && count < cap; ++i) {
    if (!atImport(css, n, i)) {
      continue;
    }
    size_t j = i + 7;
    while (j < n && cssSpace(css[j])) {
      ++j;
    }
    if (j + 4 <= n && (css[j] == 'u' || css[j] == 'U') && (css[j + 1] == 'r' || css[j + 1] == 'R') &&
        (css[j + 2] == 'l' || css[j + 2] == 'L')) {
      j += 3;
      while (j < n && cssSpace(css[j])) {
        ++j;
      }
      if (j < n && css[j] == '(') {
        ++j;
      }
      while (j < n && cssSpace(css[j])) {
        ++j;
      }
    }
    char quote = 0;
    if (j < n && (css[j] == '"' || css[j] == '\'')) {
      quote = css[j++];
    }
    const size_t start = j;
    while (j < n) {
      const char c = css[j];
      if (quote) {
        if (c == quote) {
          break;
        }
      } else if (c == ')' || c == ';' || cssSpace(c)) {
        break;
      }
      ++j;
    }
    if (j > start && j - start < 96) {
      char tmp[96];
      memcpy(tmp, css + start, j - start);
      tmp[j - start] = 0;
      if (char* q = strchr(tmp, '?')) {
        *q = 0;
      }
      if (char* h = strchr(tmp, '#')) {
        *h = 0;
      }
      if (endsCssName(tmp)) {
        bool dup = false;
        for (int k = 0; k < count; ++k) {
          if (strcmp(out[k], tmp) == 0) {
            dup = true;
            break;
          }
        }
        if (!dup) {
          memcpy(out[count], tmp, strlen(tmp) + 1);
          ++count;
        }
      }
    }
    if (j > i) {
      i = j;
    }
  }
  return count;
}

static bool sliceHasI(const char* s, const size_t n, const char* lit) {
  const size_t m = strlen(lit);
  if (m == 0 || n < m) {
    return false;
  }
  for (size_t i = 0; i + m <= n; ++i) {
    bool ok = true;
    for (size_t k = 0; k < m; ++k) {
      char c = s[i + k];
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
      if (c != lit[k]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return true;
    }
  }
  return false;
}

static bool parseEmphasis(const char* s, const size_t n, uint8_t& mark) {
  if (sliceHasI(s, n, "none")) {
    mark = kFaceEmNone;
    return true;
  }
  const bool open = sliceHasI(s, n, "open");
  if (sliceHasI(s, n, "double-circle") || sliceHasI(s, n, "double circle")) {
    mark = open ? kEmDoubleCircleOpen : kEmDoubleCircle;
    return true;
  }
  if (sliceHasI(s, n, "circle")) {
    mark = open ? kEmCircleOpen : kEmCircle;
    return true;
  }
  if (sliceHasI(s, n, "triangle")) {
    mark = kEmTriangle;
    return true;
  }
  if (sliceHasI(s, n, "sesame")) {
    mark = open ? kEmSesameOpen : kEmSesame;
    return true;
  }
  if (sliceHasI(s, n, "dot")) {
    mark = open ? kEmDotOpen : kEmDot;
    return true;
  }
  return false;
}

// A style token: 2 is a visible rule, 1 is an explicit none, 0 is not a style.
static uint8_t styleOf(const char* s, const size_t n) {
  if (startsWord(s, n, "none") || startsWord(s, n, "hidden")) {
    return 1;
  }
  static const char* marks[] = {"solid", "double", "dashed", "dotted", "groove", "ridge", "inset", "outset"};
  for (const char* mark : marks) {
    if (startsWord(s, n, mark)) {
      return 2;
    }
  }
  return 0;
}

// A width token. unit 1 is px, 2 is em, 3 is rem. thin, medium, and thick are 1, 3, and 5 px.
// 0px and 0em stay absent. A positive em that rounds to 0 percent is stored as 1.
static bool widthOf(const char* s, const size_t n, uint8_t& mag, uint8_t& unit) {
  if (startsWord(s, n, "thin")) {
    mag = 1;
    unit = 1;
    return true;
  }
  if (startsWord(s, n, "medium")) {
    mag = 3;
    unit = 1;
    return true;
  }
  if (startsWord(s, n, "thick")) {
    mag = 5;
    unit = 1;
    return true;
  }
  size_t i = 0;
  if (i >= n || s[i] < '0' || s[i] > '9') {
    return false;
  }
  int ip = 0;
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    if (ip < 1000) {
      ip = ip * 10 + (s[i] - '0');
    }
    ++i;
  }
  int frac = 0;
  int fracDiv = 1;
  if (i < n && s[i] == '.') {
    ++i;
    int digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (digits < 4) {
        frac = frac * 10 + (s[i] - '0');
        fracDiv *= 10;
        ++digits;
      }
      ++i;
    }
  }
  uint8_t u = 0;
  if (startsWord(s + i, n - i, "rem")) {
    u = 3;
  } else if (startsWord(s + i, n - i, "em")) {
    u = 2;
  } else if (startsWord(s + i, n - i, "px")) {
    u = 1;
  } else {
    return false;
  }
  if (u == 1) {
    int v = ip > 255 ? 255 : ip;
    if (frac * 2 >= fracDiv && v < 255) {
      ++v;
    }
    if (v == 0) {
      return false;
    }
    mag = static_cast<uint8_t>(v);
    unit = 1;
    return true;
  }
  if (ip == 0 && frac == 0) {
    return false;
  }
  int pct = 255;
  if (ip < 3) {
    const long num = (static_cast<long>(ip) * fracDiv + frac) * 100;
    pct = static_cast<int>((num + fracDiv / 2) / fracDiv);
    if (pct < 1) {
      pct = 1;
    } else if (pct > 255) {
      pct = 255;
    }
  }
  mag = static_cast<uint8_t>(pct);
  unit = u;
  return true;
}

static void nextCssWord(const char* s, const size_t n, size_t& i, size_t& b, size_t& e) {
  while (i < n && (faceSpace(s[i]) || s[i] == ',')) {
    ++i;
  }
  b = i;
  while (i < n && !faceSpace(s[i]) && s[i] != ',' && s[i] != ';' && s[i] != '!' && s[i] != '}') {
    ++i;
  }
  e = i;
}

// 1 value fills every side. 2 is top/bottom then left/right. 3 is top, left/right,
// bottom. 4 is top, right, bottom, left. which >= 0 writes the first value on that side.
static void applyQuad(uint8_t dst[4], const int which, const uint8_t* vals, const int n) {
  if (n <= 0) {
    return;
  }
  if (which >= 0 && which < 4) {
    dst[which] = vals[0];
    return;
  }
  if (n == 1) {
    dst[0] = dst[1] = dst[2] = dst[3] = vals[0];
  } else if (n == 2) {
    dst[0] = dst[2] = vals[0];
    dst[1] = dst[3] = vals[1];
  } else if (n == 3) {
    dst[0] = vals[0];
    dst[1] = dst[3] = vals[1];
    dst[2] = vals[2];
  } else {
    dst[0] = vals[0];
    dst[1] = vals[1];
    dst[2] = vals[2];
    dst[3] = vals[3];
  }
}

// kind 0 is a style list, 1 is a width list, 2 is a border shorthand that carries
// one style and one width. which < 0 is the four-side form. A later property wins.
static void noteBorder(uint8_t side[4], uint8_t wide[4], uint8_t wunit[4], const int which, const int kind,
                       const char* s, const size_t n) {
  uint8_t styles[4]{};
  uint8_t widths[4]{};
  uint8_t units[4]{};
  int ns = 0;
  int nw = 0;
  size_t i = 0;
  while (i < n) {
    size_t b = 0;
    size_t e = 0;
    nextCssWord(s, n, i, b, e);
    if (b == e) {
      break;
    }
    const uint8_t st = styleOf(s + b, e - b);
    uint8_t mag = 0;
    uint8_t unit = 0;
    const bool isW = widthOf(s + b, e - b, mag, unit);
    if (kind == 2) {
      if (st != 0 && ns == 0) {
        styles[ns++] = st;
      }
      if (isW && nw == 0) {
        widths[0] = mag;
        units[0] = unit;
        nw = 1;
      }
      continue;
    }
    if (kind == 0 && st != 0 && ns < 4) {
      styles[ns++] = st;
    } else if (kind == 1 && isW && nw < 4) {
      widths[nw] = mag;
      units[nw] = unit;
      ++nw;
    }
  }
  if (kind == 2) {
    if (ns > 0) {
      applyQuad(side, which, styles, 1);
    }
    if (nw > 0) {
      applyQuad(wide, which, widths, 1);
      applyQuad(wunit, which, units, 1);
    }
    return;
  }
  if (kind == 0 && ns > 0) {
    applyQuad(side, which, styles, ns);
  }
  if (kind == 1 && nw > 0) {
    applyQuad(wide, which, widths, nw);
    applyQuad(wunit, which, units, nw);
  }
}

// 1 is no fill. 2–255 is the gray, with black clamped to 2 so 0 can stay "absent".
static int hexDigit(const char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static bool parseShade(const char* s, const size_t n, uint8_t& shade) {
  size_t i = 0;
  while (i < n && faceSpace(s[i])) {
    ++i;
  }
  if (startsWord(s + i, n - i, "transparent") || startsWord(s + i, n - i, "none") ||
      startsWord(s + i, n - i, "white")) {
    shade = 1;
    return true;
  }
  if (startsWord(s + i, n - i, "black")) {
    shade = 2;
    return true;
  }
  for (size_t k = i; k < n; ++k) {
    if (s[k] != '#') {
      continue;
    }
    int h[6];
    int hn = 0;
    for (size_t p = k + 1; p < n && hn < 6; ++p) {
      const int v = hexDigit(s[p]);
      if (v < 0) {
        break;
      }
      h[hn++] = v;
    }
    int r = 0;
    int g = 0;
    int b = 0;
    if (hn == 3) {
      r = h[0] * 17;
      g = h[1] * 17;
      b = h[2] * 17;
    } else if (hn == 6) {
      r = h[0] * 16 + h[1];
      g = h[2] * 16 + h[3];
      b = h[4] * 16 + h[5];
    } else {
      continue;
    }
    int gray = (r * 30 + g * 59 + b * 11) / 100;
    if (gray >= 250) {
      shade = 1;
      return true;
    }
    if (gray < 2) {
      gray = 2;
    }
    shade = static_cast<uint8_t>(gray);
    return true;
  }
  return false;
}

// The first side that carries a width. onlyVisible skips a side whose style is not a rule.
static void firstWidth(const uint8_t side[4], const uint8_t wide[4], const uint8_t wunit[4], const bool onlyVisible,
                       uint8_t& mag, uint8_t& unit) {
  mag = 0;
  unit = 0;
  for (int i = 0; i < 4; ++i) {
    if (onlyVisible && side[i] != 2) {
      continue;
    }
    if (wunit[i] == 0 || wide[i] == 0) {
      continue;
    }
    mag = wide[i];
    unit = wunit[i];
    return;
  }
}

// px clears an em width, and the reverse. A later rule of the same selector can then merge.
static void storeWidth(FacePaint& out, const uint8_t mag, const uint8_t unit) {
  if (mag == 0) {
    return;
  }
  if (unit == 1) {
    out.rulePx = mag;
    out.ruleEm = 0;
    out.ruleRoot = 0;
  } else if (unit == 2 || unit == 3) {
    out.ruleEm = mag;
    out.rulePx = 0;
    out.ruleRoot = unit == 3 ? 1 : 0;
  }
}

// Four visible sides are a full 囲み罫. Two or three are a 囲み罫 of those sides.
// One visible side is 傍線 when the declaration has no 圏点, in the traditional slot.
// An explicit none is box 1. A width with no style is kept so a later style rule can use it.
static void finishBorder(const uint8_t side[4], const uint8_t wide[4], const uint8_t wunit[4], FacePaint& out) {
  int vis = 0;
  int none = 0;
  uint8_t mask = 0;
  for (int i = 0; i < 4; ++i) {
    if (side[i] == 2) {
      ++vis;
      mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(kBorderTop << i));
    } else if (side[i] == 1) {
      ++none;
    }
  }
  uint8_t mag = 0;
  uint8_t unit = 0;
  if (vis == 4) {
    out.box = 2;
    out.ruleSides = 0;
    firstWidth(side, wide, wunit, true, mag, unit);
    storeWidth(out, mag, unit);
  } else if (vis == 2 || vis == 3) {
    out.box = 2;
    out.ruleSides = mask;
    firstWidth(side, wide, wunit, true, mag, unit);
    storeWidth(out, mag, unit);
  } else if (vis == 0 && none > 0) {
    out.box = 1;
  } else if (vis == 1 && out.emphasis == 0) {
    out.emphasis = kEmLine;
    out.ruleSides = 0;
    firstWidth(side, wide, wunit, true, mag, unit);
    storeWidth(out, mag, unit);
  } else if (vis == 0 && none == 0) {
    firstWidth(side, wide, wunit, false, mag, unit);
    storeWidth(out, mag, unit);
  }
}

// Longer names first so `border` does not swallow `border-right`. -1 is the shorthand.
// kind 0 is style, 1 is width, 2 is the border shorthand.
static bool matchBorder(const char* s, const size_t n, const size_t i, size_t& at, int& which, int& kind) {
  static const char* keys[] = {
      "border-top-width",    "border-right-width",  "border-bottom-width", "border-left-width",
      "border-top-style",    "border-right-style",  "border-bottom-style", "border-left-style",
      "border-width",        "border-style",        "border-top",          "border-right",
      "border-bottom",       "border-left",         "border",
  };
  static const int sides[] = {0, 1, 2, 3, 0, 1, 2, 3, -1, -1, 0, 1, 2, 3, -1};
  static const int kinds[] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 2, 2, 2, 2, 2};
  for (int k = 0; k < 15; ++k) {
    if (faceKey(s, n, i, keys[k], at)) {
      which = sides[k];
      kind = kinds[k];
      return true;
    }
  }
  return false;
}

static bool tokenPrefix(const char* s, const size_t n, const char* lit) {
  const size_t m = strlen(lit);
  if (n < m) {
    return false;
  }
  for (size_t i = 0; i < m; ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != lit[i]) {
      return false;
    }
  }
  return true;
}

// The first generic family wins. sans-serif-ja is sans; serif-ja is serif.
static bool parseFamily(const char* s, const size_t n, uint8_t& family) {
  size_t i = 0;
  while (i < n) {
    while (i < n && !faceChar(static_cast<unsigned char>(s[i]))) {
      ++i;
    }
    if (i >= n) {
      break;
    }
    const size_t begin = i;
    while (i < n && faceChar(static_cast<unsigned char>(s[i]))) {
      ++i;
    }
    const char* tok = s + begin;
    const size_t len = i - begin;
    if (tokenPrefix(tok, len, "sans") || tokenPrefix(tok, len, "gothic")) {
      family = kFaceSans;
      return true;
    }
    if (tokenPrefix(tok, len, "serif")) {
      family = kFaceSerif;
      return true;
    }
  }
  return false;
}

void readFaceDecls(const char* s, const size_t n, FacePaint& out) {
  if (!s || n == 0) {
    return;
  }
  uint8_t side[4] = {};
  uint8_t wide[4] = {};
  uint8_t wunit[4] = {};
  for (size_t i = 0; i < n; ++i) {
    size_t at = 0;
    if (faceKey(s, n, i, "font-family", at)) {
      const size_t end = declEnd(s, n, at);
      uint8_t fam = 0;
      if (parseFamily(s + at, end - at, fam)) {
        out.family = fam;
      }
      i = end > i ? end - 1 : i;
      continue;
    }
    if (faceKey(s, n, i, "font-weight", at)) {
      const size_t end = declEnd(s, n, at);
      uint8_t w = 0;
      if (parseWeight(s + at, end - at, w)) {
        out.weight = w;
      }
      i = end > i ? end - 1 : i;
      continue;
    }
    if (faceKey(s, n, i, "font-size", at)) {
      const size_t end = declEnd(s, n, at);
      uint8_t p = 0;
      uint8_t r = 0;
      if (parseSize(s + at, end - at, p, r)) {
        out.pct = p;
        out.root = r;
      }
      i = end > i ? end - 1 : i;
      continue;
    }
    if (faceKey(s, n, i, "text-emphasis-style", at) || faceKey(s, n, i, "-webkit-text-emphasis-style", at) ||
        faceKey(s, n, i, "text-emphasis", at)) {
      const size_t end = declEnd(s, n, at);
      uint8_t mark = 0;
      if (parseEmphasis(s + at, end - at, mark)) {
        out.emphasis = mark;
      }
      i = end > i ? end - 1 : i;
      continue;
    }
    int which = 0;
    int kind = 0;
    if (matchBorder(s, n, i, at, which, kind)) {
      const size_t end = declEnd(s, n, at);
      noteBorder(side, wide, wunit, which, kind, s + at, end - at);
      i = end > i ? end - 1 : i;
      continue;
    }
    if (faceKey(s, n, i, "background-color", at) || faceKey(s, n, i, "background", at)) {
      const size_t end = declEnd(s, n, at);
      uint8_t shade = 0;
      if (parseShade(s + at, end - at, shade)) {
        out.shade = shade;
      }
      i = end > i ? end - 1 : i;
    }
  }
  finishBorder(side, wide, wunit, out);
}

}  // namespace ts
