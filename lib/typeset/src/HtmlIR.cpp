#include "HtmlIR.h"

#include "HtmlSax.h"

#include <Utf8.h>

#include <cstdio>
#include <cstring>

namespace ts {
namespace {

bool emitAtom(const AtomSink& sink, const Atom& a, const uint32_t pos) {
  if (!sink.emit) {
    return true;
  }
  return sink.emit(sink.ctx, a, pos);
}

bool eq(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }

bool isSkip(const char* name) {
  return eq(name, "script") || eq(name, "style") || eq(name, "head") || eq(name, "rp");
}

bool isPara(const char* name) { return eq(name, "p") || eq(name, "li") || eq(name, "tr"); }

bool isHeading(const char* name) { return name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0; }

bool attrToken(const HtmlSax& sax, const char* key, const char* token) {
  char v[80];
  if (!sax.attr(key, v, sizeof(v))) {
    return false;
  }
  const size_t tn = strlen(token);
  const char* s = v;
  while (*s) {
    while (*s == ' ') {
      ++s;
    }
    const char* e = s;
    while (*e && *e != ' ') {
      ++e;
    }
    if (static_cast<size_t>(e - s) == tn && memcmp(s, token, tn) == 0) {
      return true;
    }
    s = e;
  }
  return false;
}

// Contents block. 電書協 puts this on body (p-toc). Harry's volume list is
// <section class="toc" epub:type="toc">, and the prefix is stored as "type".
bool isTocContainer(const HtmlSax& sax) {
  const bool box = eq(sax.name, "body") || eq(sax.name, "section") || eq(sax.name, "nav") ||
                   eq(sax.name, "div") || eq(sax.name, "ol") || eq(sax.name, "ul");
  if (!box) {
    return false;
  }
  if (sax.hasClass("toc") || sax.hasClass("p-toc") || sax.hasClassPrefix("p-toc")) {
    return true;
  }
  return attrToken(sax, "type", "toc");
}

// Harry's series page is body.cover, with p.tochead and p.toc1 outside a section.
bool isTocParaClass(const HtmlSax& sax) {
  return sax.hasClassPrefix("toc") || sax.hasClass("p-toc") || sax.hasClassPrefix("p-toc");
}

int startEmFromClass(const HtmlSax& sax) {
  char cls[256];
  if (!sax.attr("class", cls, sizeof(cls))) {
    return -1;
  }
  const char* s = cls;
  while (*s) {
    while (*s == ' ') {
      ++s;
    }
    const char* e = s;
    while (*e && *e != ' ') {
      ++e;
    }
    auto take = [&](const char* prefix) -> int {
      const size_t pn = strlen(prefix);
      if (static_cast<size_t>(e - s) <= pn || memcmp(s, prefix, pn) != 0) {
        return -1;
      }
      const char* n = s + pn;
      const char* ne = e;
      if (ne - n >= 2 && ne[-2] == 'e' && ne[-1] == 'm') {
        ne -= 2;
      }
      int v = 0;
      if (n == ne) {
        return -1;
      }
      while (n < ne) {
        if (*n < '0' || *n > '9') {
          return -1;
        }
        v = v * 10 + (*n - '0');
        ++n;
      }
      if (v >= 0 && v <= 20) {
        return v;
      }
      return -1;
    };
    int v = take("h-indent-");
    if (v < 0) {
      v = take("start-");
    }
    if (v >= 0) {
      return v;
    }
    s = e;
  }
  return -1;
}

// Integer ems of an 電書協 class such as m-top-1em. 0em50 is 0. Missing is -1.
int classEm(const HtmlSax& sax, const char* prefix) {
  char cls[256];
  if (!sax.attr("class", cls, sizeof(cls))) {
    return -1;
  }
  const size_t pn = strlen(prefix);
  const char* s = cls;
  while (*s) {
    while (*s == ' ') {
      ++s;
    }
    const char* e = s;
    while (*e && *e != ' ') {
      ++e;
    }
    if (static_cast<size_t>(e - s) > pn && memcmp(s, prefix, pn) == 0) {
      const char* n = s + pn;
      if (n < e && *n >= '0' && *n <= '9') {
        int v = 0;
        while (n < e && *n >= '0' && *n <= '9') {
          v = v * 10 + (*n - '0');
          ++n;
        }
        return v > 20 ? 20 : v;
      }
    }
    s = e;
  }
  return -1;
}

// Percent of an em. 0em50 is 50, 1em is 100, 1em25 is 125. Missing is -1.
int classEmPct(const HtmlSax& sax, const char* prefix) {
  char cls[256];
  if (!sax.attr("class", cls, sizeof(cls))) {
    return -1;
  }
  const size_t pn = strlen(prefix);
  const char* s = cls;
  while (*s) {
    while (*s == ' ') {
      ++s;
    }
    const char* e = s;
    while (*e && *e != ' ') {
      ++e;
    }
    if (static_cast<size_t>(e - s) > pn && memcmp(s, prefix, pn) == 0) {
      const char* n = s + pn;
      if (n < e && *n >= '0' && *n <= '9') {
        int whole = 0;
        while (n < e && *n >= '0' && *n <= '9') {
          whole = whole * 10 + (*n - '0');
          ++n;
        }
        if (static_cast<size_t>(e - n) >= 2 && n[0] == 'e' && n[1] == 'm') {
          n += 2;
          int frac = 0;
          if (n == e) {
            frac = 0;
          } else if (n + 2 == e && n[0] >= '0' && n[0] <= '9' && n[1] >= '0' && n[1] <= '9') {
            frac = (n[0] - '0') * 10 + (n[1] - '0');
          } else {
            s = e;
            continue;
          }
          int pct = whole * 100 + frac;
          if (pct > 255) {
            pct = 255;
          }
          return pct;
        }
      }
    }
    s = e;
  }
  return -1;
}

// m-left / m-right is the space before and after a line. A 小見出し is that div.
bool sideMargin(const HtmlSax& sax) {
  return sax.hasClassPrefix("m-left-") || sax.hasClassPrefix("m-right-");
}

WritingMode modeFromSax(const HtmlSax& sax) {
  if (sax.hasClass("hltr") || sax.hasClassPrefix("hltr") || sax.hasClass("horizontal-tb")) {
    return WritingMode::HorizontalTb;
  }
  if (sax.hasClass("vrtl") || sax.hasClassPrefix("vrtl") || sax.hasClass("vertical-rl")) {
    return WritingMode::VerticalRl;
  }
  char style[192];
  if (sax.attr("style", style, sizeof(style))) {
    if (strstr(style, "horizontal-tb") || strstr(style, "lr-tb") || strstr(style, "rl-tb")) {
      return WritingMode::HorizontalTb;
    }
    if (strstr(style, "vertical-rl") || strstr(style, "tb-rl") || strstr(style, "vertical-lr")) {
      return WritingMode::VerticalRl;
    }
  }
  return WritingMode::VerticalRl;
}

bool modeSpecified(const HtmlSax& sax) {
  return sax.hasClass("hltr") || sax.hasClassPrefix("hltr") || sax.hasClass("horizontal-tb") || sax.hasClass("vrtl") ||
         sax.hasClassPrefix("vrtl") || sax.hasClass("vertical-rl") || sax.attrContains("style", "writing-mode") ||
         sax.attrContains("style", "horizontal-tb") || sax.attrContains("style", "vertical-rl");
}

void appendTitleCp(char* buf, uint8_t& n, const size_t cap, const uint32_t cp) {
  unsigned char tmp[4];
  int tn = 0;
  if (cp < 0x80) {
    tmp[tn++] = static_cast<unsigned char>(cp);
  } else if (cp < 0x800) {
    tmp[tn++] = static_cast<unsigned char>(0xC0 | (cp >> 6));
    tmp[tn++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    tmp[tn++] = static_cast<unsigned char>(0xE0 | (cp >> 12));
    tmp[tn++] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
    tmp[tn++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    tmp[tn++] = static_cast<unsigned char>(0xF0 | (cp >> 18));
    tmp[tn++] = static_cast<unsigned char>(0x80 | ((cp >> 12) & 0x3F));
    tmp[tn++] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
    tmp[tn++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
  } else {
    return;
  }
  if (static_cast<size_t>(n) + static_cast<size_t>(tn) >= cap) {
    return;
  }
  memcpy(buf + n, tmp, static_cast<size_t>(tn));
  n = static_cast<uint8_t>(n + tn);
  buf[n] = 0;
}

void appendUtf8(uint32_t* cps, uint8_t& n, const uint8_t cap, const char* s, const uint16_t slen) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  const unsigned char* end = p + slen;
  while (p < end && n < cap) {
    const unsigned char* next = p;
    const uint32_t cp = utf8NextCodepoint(&next);
    if (next == p) {
      break;
    }
    p = next;
    if (cp == 0) {
      break;
    }
    cps[n++] = cp;
  }
}

bool isWsCp(const uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0; }

Atom chAtom(const uint32_t cp, const uint32_t* ruby, const uint8_t rubyN, const uint8_t emphasis) {
  Atom a{};
  a.kind = AtomKind::Ch;
  a.cp = cp;
  a.rubyCount = rubyN > 8 ? 8 : rubyN;
  for (uint8_t i = 0; i < a.rubyCount; ++i) {
    a.ruby[i] = ruby[i];
  }
  a.emphasis = emphasis;
  return a;
}

bool isPlaceholder(const uint32_t cp) {
  switch (cp) {
    case 0x3013:  // 〓
    case 0xFFFD:
    case 0x25A0:  // ■
    case 0x25A1:  // □
    case 0x25CF:  // ●
    case 0x25CB:  // ○
    case 0x25C6:  // ◆
    case 0x25C7:  // ◇
    case 0x203B:  // ※
    case 0xFF0A:  // ＊
    case '*':
    case 0x2605:  // ★
    case 0x2606:  // ☆
    case 0xFF1F:  // ？
    case '?':
    case 0x25CE:  // ◎
      return true;
    default:
      return false;
  }
}

uint32_t cidChar(const int cid) {
  switch (cid) {
    case 13803:
      return 0x20B9F;  // 𠮟
    case 13706:
      return 0x20BB7;  // 𠮷
    case 20315:
      return 0x29E3D;  // 𩸽
    default:
      return 0;
  }
}

int cidFromSrc(const char* src) {
  if (!src) {
    return 0;
  }
  for (const char* p = src; *p; ++p) {
    const char c0 = p[0] | 0x20;
    const char c1 = p[1] ? static_cast<char>(p[1] | 0x20) : 0;
    const char c2 = p[2] ? static_cast<char>(p[2] | 0x20) : 0;
    if (c0 == 'c' && c1 == 'i' && c2 == 'd') {
      const char* n = p + 3;
      if (*n == '-' || *n == '_') {
        ++n;
      }
      if (*n < '0' || *n > '9') {
        continue;
      }
      int v = 0;
      while (*n >= '0' && *n <= '9') {
        v = v * 10 + (*n - '0');
        ++n;
      }
      return v;
    }
  }
  return 0;
}

uint8_t emphasisOf(const HtmlSax& sax) {
  if (sax.hasClass("em-sesame-open")) {
    return kEmSesameOpen;
  }
  if (sax.hasClass("em-sesame") || sax.hasClass("em")) {
    return kEmSesame;
  }
  if (sax.hasClass("em-dot-open")) {
    return kEmDotOpen;
  }
  if (sax.hasClass("em-dot")) {
    return kEmDot;
  }
  if (sax.hasClass("em-circle-open")) {
    return kEmCircleOpen;
  }
  if (sax.hasClass("em-double-circle-open")) {
    return kEmDoubleCircleOpen;
  }
  if (sax.hasClass("em-double-circle")) {
    return kEmDoubleCircle;
  }
  if (sax.hasClass("em-circle")) {
    return kEmCircle;
  }
  if (sax.hasClass("em-triangle")) {
    return kEmTriangle;
  }
  if (sax.hasClass("em-line") || sax.hasClassPrefix("em-line")) {
    return kEmLine;
  }
  return 0;
}

int heightEmOf(const HtmlSax& sax) {
  char cls[256];
  if (!sax.attr("class", cls, sizeof(cls))) {
    return -1;
  }
  const char* s = cls;
  while (*s) {
    while (*s == ' ') {
      ++s;
    }
    const char* e = s;
    while (*e && *e != ' ') {
      ++e;
    }
    const char* prefix = "height-";
    const size_t pn = 7;
    if (static_cast<size_t>(e - s) > pn && memcmp(s, prefix, pn) == 0) {
      int v = 0;
      const char* n = s + pn;
      if (n < e && *n >= '0' && *n <= '9') {
        while (n < e && *n >= '0' && *n <= '9') {
          v = v * 10 + (*n - '0');
          ++n;
        }
        if (v > 0 && v <= 8) {
          return v;
        }
      }
    }
    s = e;
  }
  return -1;
}

bool sliceHas(const char* s, const size_t n, const char* lit) {
  const size_t m = strlen(lit);
  if (m == 0 || n < m) {
    return false;
  }
  for (size_t i = 0; i + m <= n; ++i) {
    if (memcmp(s + i, lit, m) == 0) {
      return true;
    }
  }
  return false;
}

WritingMode modeInSlice(const char* s, const size_t n, bool& found) {
  WritingMode mode = WritingMode::VerticalRl;
  if (sliceHas(s, n, "hltr") || sliceHas(s, n, "horizontal-tb") || sliceHas(s, n, "lr-tb") ||
      sliceHas(s, n, "rl-tb")) {
    mode = WritingMode::HorizontalTb;
    found = true;
  }
  if (sliceHas(s, n, "vrtl") || sliceHas(s, n, "vertical-rl") || sliceHas(s, n, "vertical-lr") ||
      sliceHas(s, n, "tb-rl")) {
    mode = WritingMode::VerticalRl;
    found = true;
  }
  return mode;
}

}  // namespace


void splitReading(const uint32_t* rt, const uint8_t rtN, const uint8_t baseN, uint32_t* out, uint8_t* outCount) {
  for (uint8_t i = 0; i < baseN; ++i) {
    outCount[i] = 0;
  }
  if (baseN == 0) {
    return;
  }
  if (rtN == baseN) {
    for (uint8_t i = 0; i < baseN; ++i) {
      out[i * 4] = rt[i];
      outCount[i] = 1;
    }
    return;
  }
  const uint8_t n = rtN > 4 ? 4 : rtN;
  outCount[0] = n;
  for (uint8_t i = 0; i < n; ++i) {
    out[i] = rt[i];
  }
}

static bool htmlFromSax(HtmlSax& sax, const AtomSink sink, HtmlIRResult* result);

bool sniffWritingMode(const char* data, const size_t len, WritingMode& mode) {
  if (!data || len == 0) {
    return false;
  }
  const size_t cap = len < 16384 ? len : 16384;
  bool cssFound = false;
  WritingMode cssMode = WritingMode::VerticalRl;
  size_t i = 0;
  while (i + 5 < cap) {
    if (data[i] != '<') {
      ++i;
      continue;
    }
    if (i + 6 <= cap && memcmp(data + i, "<style", 6) == 0) {
      size_t j = i + 6;
      while (j + 8 <= cap && memcmp(data + j, "</style>", 8) != 0) {
        ++j;
      }
      bool found = false;
      const WritingMode m = modeInSlice(data + i, j - i, found);
      if (found) {
        cssFound = true;
        cssMode = m;
      }
      i = j;
      continue;
    }
    const bool html = i + 5 <= cap && memcmp(data + i, "<html", 5) == 0;
    const bool body = i + 5 <= cap && memcmp(data + i, "<body", 5) == 0;
    if (html || body) {
      size_t j = i;
      while (j < cap && data[j] != '>') {
        ++j;
      }
      bool found = false;
      const WritingMode m = modeInSlice(data + i, j - i, found);
      if (found) {
        mode = m;
        return true;
      }
      i = j;
      continue;
    }
    ++i;
  }
  if (cssFound) {
    mode = cssMode;
    return true;
  }
  return false;
}

bool textOutsideTagsIsLong(const char* data, const size_t len) {
  if (!data || len == 0) {
    return false;
  }
  uint32_t chars = 0;
  bool inTag = false;
  int skip = 0;  // 1 style, 2 script
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(data[i]);
    if (skip != 0) {
      const char* end = skip == 1 ? "</style" : "</script";
      const size_t en = skip == 1 ? 7 : 8;
      if (i + en <= len && memcmp(data + i, end, en) == 0) {
        skip = 0;
      }
      continue;
    }
    if (c == '<') {
      if (i + 6 <= len && (memcmp(data + i, "<style", 6) == 0 || memcmp(data + i, "<STYLE", 6) == 0)) {
        skip = 1;
        continue;
      }
      if (i + 7 <= len && (memcmp(data + i, "<script", 7) == 0 || memcmp(data + i, "<SCRIPT", 7) == 0)) {
        skip = 2;
        continue;
      }
      inTag = true;
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (inTag || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      continue;
    }
    if ((c & 0xC0) == 0x80) {
      continue;
    }
    if (++chars >= 2000) {
      return true;
    }
  }
  return false;
}

bool htmlToAtoms(const char* data, const size_t len, AtomSink sink, HtmlIRResult* result) {
  HtmlSax sax;
  sax.bind(data, len);
  sink.longText = textOutsideTagsIsLong(data, len);
  return htmlFromSax(sax, sink, result);
}

bool htmlToAtomsPull(int (*read)(void*, char*, int), void* ctx, const AtomSink sink, HtmlIRResult* result) {
  HtmlSax sax;
  sax.bindPull(read, ctx);
  return htmlFromSax(sax, sink, result);
}

static void faceFor(const HtmlSax& sax, const AtomSink& sink, const uint8_t parentScale, const uint8_t parentBold,
                    uint8_t& scale, uint8_t& bold, uint8_t& cssEmph, uint8_t& boxed, uint8_t& shaded, uint8_t& ruled,
                    uint8_t& ruledEm, uint8_t& ruledRoot, uint8_t& ruledSides, const uint8_t parentFamily,
                    uint8_t& family, const bool vertical) {
  scale = parentScale == 0 ? 100 : parentScale;
  bold = parentBold;
  family = parentFamily;
  cssEmph = 0;
  boxed = 0;
  shaded = 0;
  ruled = 0;
  ruledEm = 0;
  ruledRoot = 0;
  ruledSides = 0;
  int bestW = 0;
  int bestS = 0;
  int bestE = 0;
  int bestB = 0;
  int bestH = 0;
  int bestR = 0;
  int bestSide = 0;
  int bestF = 0;
  if (eq(sax.name, "b") || eq(sax.name, "strong")) {
    bold = 1;
    bestW = 1;
  }
  if (sink.rules) {
    const uint16_t n = sink.nRules > kMaxFaceRules ? kMaxFaceRules : sink.nRules;
    for (uint16_t i = 0; i < n; ++i) {
      const FaceRule& r = sink.rules[i];
      if (r.tag[0] == 0 && r.cls[0] == 0) {
        continue;
      }
      if (r.mode == 1 && vertical) {
        continue;
      }
      if (r.mode == 2 && !vertical) {
        continue;
      }
      if (r.tag[0] && !eq(sax.name, r.tag)) {
        continue;
      }
      if (r.cls[0] && !sax.hasClass(r.cls)) {
        continue;
      }
      const int score = (r.tag[0] ? 1 : 0) + (r.cls[0] ? 10 : 0);
      if (r.weight != 0 && score >= bestW) {
        bold = r.weight == 2 ? 1 : 0;
        bestW = score;
      }
      if (r.pct != 0 && score >= bestS) {
        const int base = r.root ? 100 : (parentScale == 0 ? 100 : parentScale);
        int next = base * static_cast<int>(r.pct) / 100;
        if (next < 40) {
          next = 40;
        }
        if (next > 200) {
          next = 200;
        }
        scale = static_cast<uint8_t>(next);
        bestS = score;
      }
      if (r.emphasis != 0 && score >= bestE) {
        cssEmph = r.emphasis;
        bestE = score;
      }
      if (r.box != 0 && score >= bestB) {
        boxed = r.box;
        bestB = score;
      }
      if (r.shade != 0 && score >= bestH) {
        shaded = r.shade;
        bestH = score;
      }
      if ((r.rulePx != 0 || r.ruleEm != 0) && score >= bestR) {
        ruled = r.rulePx;
        ruledEm = r.ruleEm;
        ruledRoot = r.ruleRoot;
        bestR = score;
      }
      if (r.ruleSides != 0 && score >= bestSide) {
        ruledSides = r.ruleSides;
        bestSide = score;
      }
      if (r.family != 0 && score >= bestF) {
        family = r.family;
        bestF = score;
      }
    }
  }
  char style[180];
  if (sax.attr("style", style, sizeof(style))) {
    FacePaint paint{};
    readFaceDecls(style, strlen(style), paint);
    if (paint.weight == 2) {
      bold = 1;
    } else if (paint.weight == 1) {
      bold = 0;
    }
    if (paint.pct != 0) {
      const int base = paint.root ? 100 : (parentScale == 0 ? 100 : parentScale);
      int next = base * static_cast<int>(paint.pct) / 100;
      if (next < 40) {
        next = 40;
      }
      if (next > 200) {
        next = 200;
      }
      scale = static_cast<uint8_t>(next);
    }
    if (paint.emphasis != 0) {
      cssEmph = paint.emphasis;
    }
    if (paint.box != 0) {
      boxed = paint.box;
    }
    if (paint.shade != 0) {
      shaded = paint.shade;
    }
    if (paint.rulePx != 0 || paint.ruleEm != 0) {
      ruled = paint.rulePx;
      ruledEm = paint.ruleEm;
      ruledRoot = paint.ruleRoot;
    }
    if (paint.ruleSides != 0) {
      ruledSides = paint.ruleSides;
    }
    if (paint.family != 0) {
      family = paint.family;
    }
  }
}

static bool htmlFromSax(HtmlSax& sax, const AtomSink sink, HtmlIRResult* result) {
  HtmlIRResult local{};
  HtmlIRResult& res = result ? *result : local;

  bool inBody = false;
  int skipDepth = 0;
  int rubyDepth = 0;
  int tcyDepth = 0;
  int indent = 0;
  int indentStack[16]{};
  uint8_t indentTop = 0;
  uint32_t tcyCps[kMaxCluster]{};
  uint8_t tcyN = 0;
  uint32_t rubyBase[8]{};
  uint8_t rubyBaseN = 0;
  uint32_t rubyRt[8]{};
  uint8_t rubyRtN = 0;
  bool inRt = false;
  bool inRb = false;
  bool lastWasGap = true;
  bool lastWasCol = false;
  bool lastWasGlyph = false;
  uint32_t textCount = 0;
  char titleBuf[96]{};
  bool capturingTitle = false;
  uint8_t titleN = 0;

  auto emit = [&](const Atom& a, const uint32_t pos) -> bool {
    if (a.kind == AtomKind::Gap) {
      if (lastWasGap || lastWasCol) {
        return true;
      }
      lastWasGap = true;
      lastWasGlyph = false;
    } else if (a.kind == AtomKind::ColumnBreak) {
      lastWasCol = true;
      lastWasGap = false;
      lastWasGlyph = false;
    } else if (a.kind == AtomKind::Space) {
      lastWasGlyph = false;
    } else {
      lastWasGap = false;
      lastWasCol = false;
      lastWasGlyph = a.kind == AtomKind::Ch || a.kind == AtomKind::Tcy || a.kind == AtomKind::Group;
    }
    return emitAtom(sink, a, pos);
  };

  int depth = 0;
  // A div whose margins separate it from the lines around it. em is the 行頭.
  // after is the percent of an em that follows the block.
  struct LineBox {
    int depth;
    uint8_t em;
    uint8_t after;
  };
  LineBox lineBox[8]{};
  uint8_t lineTop = 0;
  struct EmphFrame {
    int depth;
    uint8_t prev;
  };
  EmphFrame emphFrames[8]{};
  uint8_t emphN = 0;
  uint8_t curEmph = 0;
  uint8_t curScale = 100;
  uint8_t curBold = 0;
  uint8_t curFamily = 0;
  struct FaceFrame {
    int depth;
    uint8_t scale;
    uint8_t bold;
    uint8_t shade;
    uint8_t rule;
    uint8_t ruleEm;
    uint8_t ruleRoot;
    uint8_t ruleSides;
    uint8_t family;
  };
  FaceFrame faceFrames[16]{};
  uint8_t faceN = 0;
  uint8_t curShade = 0;
  uint8_t curRule = 0;
  uint8_t curRuleEm = 0;
  uint8_t curRuleRoot = 0;
  uint8_t curRuleSides = 0;
  uint8_t curBox = 0;
  uint8_t nextBoxId = 1;
  struct BoxFrame {
    int depth;
    uint8_t prev;
  };
  BoxFrame boxFrames[16]{};
  uint8_t boxN = 0;
  auto stamp = [&](Atom& a) {
    a.emphasis = static_cast<uint8_t>((a.emphasis & kEmphasisMask) | (curBold ? kEmphasisBold : 0));
    a.startEm = curScale == 100 ? 0 : curScale;
    a.box = curBox;
    a.shade = curShade;
    a.rulePx = curRule;
    a.ruleEm = curRuleEm;
    a.ruleRoot = curRuleRoot;
    a.ruleSides = curRuleSides;
    a.family = curFamily == kFaceSans ? kFaceSans : 0;
  };
  auto emitCh = [&](const uint32_t cp, const uint32_t* ruby, const uint8_t n, const uint32_t pos) {
    Atom a = chAtom(cp, ruby, n, curEmph);
    stamp(a);
    emit(a, pos);
  };
  bool pictureBody = false;
  bool inHeading = false;
  // Depths of open contents containers. A paragraph is one column while inside,
  // or when its own class is a contents class (toc1, tochead, toc_chapter).
  int tocStack[8]{};
  uint8_t tocTop = 0;
  bool tocPara = false;
  bool sawToc = false;

  auto flushRubyPair = [&](const uint32_t pos) {
    if (rubyBaseN == 0) {
      rubyRtN = 0;
      return;
    }
    if (rubyBaseN == 1) {
      emitCh(rubyBase[0], rubyRt, rubyRtN, pos);
    } else if (rubyRtN != rubyBaseN) {
      Atom g{};
      g.kind = AtomKind::Group;
      g.tcyCount = rubyBaseN > kMaxCluster ? kMaxCluster : rubyBaseN;
      for (uint8_t i = 0; i < g.tcyCount; ++i) {
        g.tcy[i] = rubyBase[i];
      }
      g.cp = g.tcy[0];
      g.rubyCount = rubyRtN > 8 ? 8 : rubyRtN;
      for (uint8_t i = 0; i < g.rubyCount; ++i) {
        g.ruby[i] = rubyRt[i];
      }
      g.emphasis = curEmph;
      stamp(g);
      emit(g, pos);
      for (uint8_t i = g.tcyCount; i < rubyBaseN; ++i) {
        emitCh(rubyBase[i], nullptr, 0, pos);
      }
    } else {
      uint32_t packed[8 * 4]{};
      uint8_t counts[8]{};
      const uint8_t bases = rubyBaseN > 8 ? 8 : rubyBaseN;
      splitReading(rubyRt, rubyRtN, bases, packed, counts);
      for (uint8_t i = 0; i < bases; ++i) {
        emitCh(rubyBase[i], packed + i * 4, counts[i], pos);
      }
    }
    rubyBaseN = 0;
    rubyRtN = 0;
  };

  auto emitText2 = [&](const char* s, const uint16_t n, const uint32_t pos) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* end = p + n;
    while (p < end) {
      const unsigned char* next = p;
      const uint32_t cp = utf8NextCodepoint(&next);
      if (next == p) {
        break;
      }
      p = next;
      if (cp == 0 || cp == '\n' || cp == '\r' || cp == '\t') {
        continue;
      }
      if (skipDepth > 0) {
        continue;
      }
      if (rubyDepth > 0) {
        if (inRt) {
          if (rubyRtN < 8) {
            rubyRt[rubyRtN++] = cp;
          }
        } else if (!isWsCp(cp)) {
          if (rubyBaseN < 8) {
            rubyBase[rubyBaseN++] = cp;
          }
        }
        continue;
      }
      if (tcyDepth > 0) {
        if (!isWsCp(cp) && tcyN < kMaxCluster) {
          tcyCps[tcyN++] = cp;
        }
        continue;
      }
      if (cp == ' ' || cp == 0xA0) {
        if (lastWasGlyph) {
          Atom sp{};
          sp.kind = AtomKind::Space;
          emit(sp, pos);
        }
        continue;
      }
      ++textCount;
      if (capturingTitle) {
        unsigned char tmp[4];
        int tn = 0;
        if (cp < 0x80) {
          tmp[tn++] = static_cast<unsigned char>(cp);
        } else if (cp < 0x800) {
          tmp[tn++] = static_cast<unsigned char>(0xC0 | (cp >> 6));
          tmp[tn++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          tmp[tn++] = static_cast<unsigned char>(0xE0 | (cp >> 12));
          tmp[tn++] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
          tmp[tn++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
        }
        if (titleN + tn < sizeof(titleBuf) - 1) {
          memcpy(titleBuf + titleN, tmp, static_cast<size_t>(tn));
          titleN = static_cast<uint8_t>(titleN + tn);
          titleBuf[titleN] = 0;
        }
      }
      emitCh(cp, nullptr, 0, pos);
    }
  };
  (void)appendUtf8;

  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Text) {
      emitText2(sax.text, sax.ntext, sax.pos);
      continue;
    }
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (sax.kind == HtmlSax::Kind::Start && !isSkip(sax.name) && skipDepth == 0) {
        ++depth;
        uint8_t scale = curScale;
        uint8_t bold = curBold;
        uint8_t cssEmph = 0;
        uint8_t boxed = 0;
        uint8_t shaded = 0;
        uint8_t ruled = 0;
        uint8_t ruledEm = 0;
        uint8_t ruledRoot = 0;
        uint8_t ruledSides = 0;
        uint8_t family = curFamily;
        faceFor(sax, sink, curScale, curBold, scale, bold, cssEmph, boxed, shaded, ruled, ruledEm, ruledRoot,
                ruledSides, curFamily, family, res.mode != WritingMode::HorizontalTb);
        uint8_t shade = curShade;
        uint8_t rule = curRule;
        uint8_t ruleEm = curRuleEm;
        uint8_t ruleRoot = curRuleRoot;
        uint8_t ruleSides = curRuleSides;
        if (ruled != 0 || ruledEm != 0) {
          rule = ruled;
          ruleEm = ruledEm;
          ruleRoot = ruledRoot;
        }
        if (ruledSides != 0) {
          ruleSides = ruledSides;
        }
        if (shaded == 1) {
          shade = 0;
        } else if (shaded >= 2) {
          shade = shaded;
        }
        uint8_t mark = curEmph;
        if (cssEmph == kFaceEmNone) {
          mark = 0;
        } else if (cssEmph != 0) {
          mark = cssEmph;
        }
        const uint8_t cls = emphasisOf(sax);
        if (cls != 0) {
          mark = cls;
        } else if (eq(sax.name, "em") && (cssEmph == 0 || cssEmph == kFaceEmNone)) {
          mark = kEmSesame;
        }
        if (mark != curEmph && emphN < 8) {
          emphFrames[emphN].depth = depth;
          emphFrames[emphN].prev = curEmph;
          ++emphN;
          curEmph = mark;
        }
        if (boxed == 2 && curBox == 0 && boxN < 16) {
          boxFrames[boxN].depth = depth;
          boxFrames[boxN].prev = curBox;
          ++boxN;
          curBox = nextBoxId;
          nextBoxId = nextBoxId == 255 ? 1 : static_cast<uint8_t>(nextBoxId + 1);
        }
        if ((scale != curScale || bold != curBold || shade != curShade || rule != curRule || ruleEm != curRuleEm ||
             ruleRoot != curRuleRoot || ruleSides != curRuleSides || family != curFamily) &&
            faceN < 16) {
          faceFrames[faceN].depth = depth;
          faceFrames[faceN].scale = curScale;
          faceFrames[faceN].bold = curBold;
          faceFrames[faceN].shade = curShade;
          faceFrames[faceN].rule = curRule;
          faceFrames[faceN].ruleEm = curRuleEm;
          faceFrames[faceN].ruleRoot = curRuleRoot;
          faceFrames[faceN].ruleSides = curRuleSides;
          faceFrames[faceN].family = curFamily;
          ++faceN;
          curScale = scale;
          curBold = bold;
          curShade = shade;
          curRule = rule;
          curRuleEm = ruleEm;
          curRuleRoot = ruleRoot;
          curRuleSides = ruleSides;
          curFamily = family;
        }
      }
      if (eq(sax.name, "html") || eq(sax.name, "body")) {
        if (modeSpecified(sax)) {
          res.mode = modeFromSax(sax);
          res.hasMode = true;
        }
        if (eq(sax.name, "body")) {
          inBody = true;
          if (sax.hasClass("p-image") || sax.hasClass("p-cover") || sax.hasClassPrefix("p-tobira")) {
            pictureBody = true;
          }
        }
      }
      char id[64];
      if (sink.anchor && sax.attr("id", id, sizeof(id)) && id[0]) {
        sink.anchor(sink.ctx, id);
      } else if (sink.anchor && eq(sax.name, "a") && sax.attr("name", id, sizeof(id)) && id[0]) {
        sink.anchor(sink.ctx, id);
      }
      if (isSkip(sax.name)) {
        if (sax.kind == HtmlSax::Kind::Start) {
          ++skipDepth;
        }
        continue;
      }
      if (skipDepth > 0) {
        continue;
      }
      if (sax.kind == HtmlSax::Kind::Start && isTocContainer(sax) && tocTop < 8) {
        tocStack[tocTop++] = depth;
        sawToc = true;
      }
      const int em = startEmFromClass(sax);
      if (sax.kind == HtmlSax::Kind::Start && indentTop < 15) {
        indentStack[indentTop++] = indent;
        if (em >= 0) {
          indent = em;
        }
      } else if (em >= 0) {
        indent = em;
      }

      if (eq(sax.name, "ruby")) {
        ++rubyDepth;
        rubyBaseN = 0;
        rubyRtN = 0;
        inRt = false;
        inRb = false;
      } else if (eq(sax.name, "rt")) {
        if (rubyDepth > 0 && rubyBaseN > 0 && rubyRtN > 0) {
          flushRubyPair(sax.pos);
        }
        inRt = true;
        rubyRtN = 0;
      } else if (eq(sax.name, "rb")) {
        inRb = true;
      } else if (eq(sax.name, "br")) {
        Atom g{};
        g.kind = AtomKind::Gap;
        emit(g, sax.pos);
      } else if (eq(sax.name, "span") &&
                 (sax.hasClass("tcy") || sax.hasClassPrefix("tcy") || sax.attrContains("style", "text-combine"))) {
        ++tcyDepth;
        tcyN = 0;
      } else if (eq(sax.name, "pagebreak")) {
        Atom brk{};
        brk.kind = AtomKind::PageBreak;
        emit(brk, sax.pos);
      } else if (isHeading(sax.name)) {
        if (titleBuf[0] == 0) {
          capturingTitle = true;
          titleN = 0;
        }
        inHeading = true;
        if (sink.heading) {
          char hid[64];
          hid[0] = 0;
          if (!sax.attr("id", hid, sizeof(hid)) || hid[0] == 0) {
            sax.attr("name", hid, sizeof(hid));
          }
          if (hid[0]) {
            sink.heading(sink.ctx, hid);
          }
        }
        Atom c{};
        c.kind = AtomKind::ColumnBreak;
        c.startEm = static_cast<uint8_t>(indent);
        emit(c, sax.pos);
      } else if (isPara(sax.name) || eq(sax.name, "div")) {
        // Vertical 行頭 is m-top. Horizontal 行頭 is m-left. The other axis only separates the line.
        const bool vert = res.mode != WritingMode::HorizontalTb;
        if (sax.kind == HtmlSax::Kind::Start && sideMargin(sax) && lineTop < 8) {
          const int head = classEm(sax, vert ? "m-top-" : "m-left-");
          const int before = classEmPct(sax, vert ? "m-right-" : "m-top-");
          const int after = classEmPct(sax, vert ? "m-left-" : "m-bottom-");
          uint8_t em = 0;
          if (head > 0) {
            em = static_cast<uint8_t>(head);
          } else if (lineTop > 0) {
            em = lineBox[lineTop - 1].em;
          }
          lineBox[lineTop].depth = depth;
          lineBox[lineTop].em = em;
          lineBox[lineTop].after = after > 0 ? static_cast<uint8_t>(after) : 0;
          ++lineTop;
          Atom c{};
          c.kind = AtomKind::ColumnBreak;
          c.startEm = em;
          c.gapBefore = before > 0 ? static_cast<uint8_t>(before) : 0;
          emit(c, sax.pos);
        } else {
          const bool inLine = lineTop > 0 && isPara(sax.name);
          const bool line =
              !sink.longText && isPara(sax.name) && textCount < 2000 && (tocTop > 0 || isTocParaClass(sax));
          if (line || inLine) {
            Atom c{};
            c.kind = AtomKind::ColumnBreak;
            if (line && indent > 0) {
              c.startEm = static_cast<uint8_t>(indent);
            } else if (inLine) {
              c.startEm = lineBox[lineTop - 1].em;
            } else {
              c.startEm = static_cast<uint8_t>(indent);
            }
            emit(c, sax.pos);
            if (line) {
              tocPara = true;
              sawToc = true;
            }
          } else if (!lastWasGap && !lastWasCol) {
            Atom g{};
            g.kind = AtomKind::Gap;
            emit(g, sax.pos);
          }
        }
      } else if (eq(sax.name, "img") || eq(sax.name, "image")) {
        char alt[64];
        alt[0] = 0;
        sax.attr("alt", alt, sizeof(alt));
        char src[240];
        src[0] = 0;
        if (!sax.attr("src", src, sizeof(src))) {
          sax.attr("href", src, sizeof(src));
        }
        const bool gaiji = sax.hasClass("gaiji") || sax.hasClassPrefix("gaiji");
        uint32_t cps[12]{};
        uint8_t nCp = 0;
        if (alt[0]) {
          const unsigned char* ap = reinterpret_cast<const unsigned char*>(alt);
          const unsigned char* end = ap + strlen(alt);
          while (ap < end && nCp < 12) {
            const unsigned char* next = ap;
            const uint32_t cp = utf8NextCodepoint(&next);
            if (next == ap || cp == 0) {
              break;
            }
            ap = next;
            cps[nCp++] = cp;
          }
        }
        const bool oneChar = nCp == 1 && !isPlaceholder(cps[0]);
        if (sink.imageLongSide) {
          if (strncmp(src, "data:", 5) == 0) {
            src[0] = 0;
          }
          uint32_t shown[24]{};
          uint8_t sn = 0;
          if (alt[0]) {
            const unsigned char* ap = reinterpret_cast<const unsigned char*>(alt);
            const unsigned char* end = ap + strlen(alt);
            while (ap < end && sn < 24) {
              const unsigned char* next = ap;
              const uint32_t cp = utf8NextCodepoint(&next);
              if (next == ap || cp == 0) {
                break;
              }
              ap = next;
              shown[sn++] = cp;
            }
          }
          uint8_t begin = 0;
          while (begin < sn && isWsCp(shown[begin])) {
            ++begin;
          }
          while (sn > begin && isWsCp(shown[sn - 1])) {
            --sn;
          }
          if (begin > 0) {
            sn = static_cast<uint8_t>(sn - begin);
            for (uint8_t i = 0; i < sn; ++i) {
              shown[i] = shown[begin + i];
            }
          }
          const bool shortChar = sn >= 1 && sn <= 2 && !isPlaceholder(shown[0]);
          bool useful = sn > 0 && sn <= 24;
          if (useful) {
            bool onlyGeta = true;
            for (uint8_t i = 0; i < sn; ++i) {
              if (shown[i] != 0x3013 && shown[i] != 0xFFFD) {
                onlyGeta = false;
                break;
              }
            }
            useful = !onlyGeta;
          }
          auto pushBase = [&](const uint32_t cp) {
            if (rubyDepth > 0) {
              if (inRt) {
                if (rubyRtN < 8) {
                  rubyRt[rubyRtN++] = cp;
                }
              } else if (!isWsCp(cp) && rubyBaseN < 8) {
                rubyBase[rubyBaseN++] = cp;
              }
              return;
            }
            if (capturingTitle) {
              appendTitleCp(titleBuf, titleN, sizeof(titleBuf), cp);
            }
            emitCh(cp, nullptr, 0, sax.pos);
          };
          auto emitAltRun = [&]() {
            uint32_t digits[24]{};
            uint8_t dN = 0;
            auto flushDigits = [&]() {
              if (dN == 0) {
                return;
              }
              if (dN >= 2 && dN <= 4 && rubyDepth == 0) {
                Atom t{};
                t.kind = AtomKind::Tcy;
                t.cp = digits[0];
                t.tcyCount = dN;
                for (uint8_t i = 0; i < dN; ++i) {
                  t.tcy[i] = digits[i];
                  if (capturingTitle) {
                    appendTitleCp(titleBuf, titleN, sizeof(titleBuf), digits[i]);
                  }
                }
                emit(t, sax.pos);
              } else {
                for (uint8_t i = 0; i < dN; ++i) {
                  pushBase(digits[i]);
                }
              }
              dN = 0;
            };
            for (uint8_t i = 0; i < sn; ++i) {
              if (shown[i] >= '0' && shown[i] <= '9' && dN < 24) {
                digits[dN++] = shown[i];
                continue;
              }
              flushDigits();
              pushBase(shown[i]);
            }
            flushDigits();
          };
          auto emitInline = [&]() {
            if (sink.picture && src[0]) {
              sink.picture(sink.ctx, src);
            }
            Atom pic{};
            pic.kind = AtomKind::Picture;
            pic.cp = 0;
            emit(pic, sax.pos);
          };
          auto emitPlate = [&]() {
            uint16_t id = 0;
            if (sink.picture && src[0]) {
              id = sink.picture(sink.ctx, src);
            }
            if (id == 0) {
              return;
            }
            Atom pic{};
            pic.kind = AtomKind::Picture;
            pic.cp = id;
            if (sink.forcePlate || (pictureBody && textCount < 2000)) {
              pic.cp |= kPictureForcePage;
            }
            emit(pic, sax.pos);
          };
          if (gaiji) {
            if (shortChar) {
              pushBase(shown[0]);
            } else if (const uint32_t mapped = cidChar(cidFromSrc(src))) {
              pushBase(mapped);
            } else if (src[0]) {
              emitInline();
            }
          } else {
            const bool sized = heightEmOf(sax) > 0;
            if ((inHeading || sized) && useful) {
              emitAltRun();
            } else if (sized && src[0]) {
              emitInline();
            } else if (src[0]) {
              const int side = sink.imageLongSide(sink.ctx, src);
              if (side >= 0 && side < 64) {
                if (useful) {
                  emitAltRun();
                }
              } else if (side >= 0 && side <= 96) {
                emitInline();
              } else {
                emitPlate();
              }
            } else if (sn > 0 && !isPlaceholder(shown[0])) {
              pushBase(shown[0]);
            }
          }
        } else if (gaiji) {
          if (oneChar) {
            emitCh(cps[0], nullptr, 0, sax.pos);
          } else if (const uint32_t mapped = cidChar(cidFromSrc(src))) {
            emitCh(mapped, nullptr, 0, sax.pos);
          }
        } else {
          uint16_t id = 0;
          if (sink.picture && src[0]) {
            id = sink.picture(sink.ctx, src);
          }
          if (id != 0) {
            Atom pic{};
            pic.kind = AtomKind::Picture;
            pic.cp = id;
            if (pictureBody || sink.forcePlate) {
              pic.cp |= kPictureForcePage;
            }
            emit(pic, sax.pos);
          } else if (pictureBody && textCount < 2000) {
            Atom pic{};
            pic.kind = AtomKind::PageBreak;
            emit(pic, sax.pos);
          } else if (nCp > 0 && (inHeading || heightEmOf(sax) > 0 || nCp <= 8)) {
            uint32_t digits[4]{};
            uint8_t dN = 0;
            auto flushDigits = [&]() {
              if (dN == 0) {
                return;
              }
              if (dN >= 2) {
                Atom t{};
                t.kind = AtomKind::Tcy;
                t.cp = digits[0];
                t.tcyCount = dN;
                t.emphasis = curEmph;
                stamp(t);
                for (uint8_t i = 0; i < dN; ++i) {
                  t.tcy[i] = digits[i];
                }
                emit(t, sax.pos);
              } else {
                emitCh(digits[0], nullptr, 0, sax.pos);
              }
              dN = 0;
            };
            for (uint8_t i = 0; i < nCp; ++i) {
              if (cps[i] >= '0' && cps[i] <= '9' && dN < 4) {
                digits[dN++] = cps[i];
                continue;
              }
              flushDigits();
              if (!isPlaceholder(cps[i])) {
                emitCh(cps[i], nullptr, 0, sax.pos);
              }
            }
            flushDigits();
          }
        }
      }
      continue;
    }
    if (sax.kind == HtmlSax::Kind::End) {
      if (isSkip(sax.name) && skipDepth > 0) {
        --skipDepth;
        continue;
      }
      if (eq(sax.name, "rt")) {
        inRt = false;
        if (rubyBaseN > 0) {
          flushRubyPair(sax.pos);
        }
      } else if (eq(sax.name, "rb")) {
        inRb = false;
      } else if (eq(sax.name, "ruby")) {
        if (rubyBaseN > 0) {
          flushRubyPair(sax.pos);
        }
        if (rubyDepth > 0) {
          --rubyDepth;
        }
        inRt = false;
        inRb = false;
      } else if (eq(sax.name, "span") && tcyDepth > 0) {
        --tcyDepth;
        if (tcyN > 0) {
          Atom t{};
          t.kind = AtomKind::Tcy;
          t.cp = tcyCps[0];
          t.tcyCount = tcyN;
          t.emphasis = curEmph;
          stamp(t);
          for (uint8_t i = 0; i < tcyN && i < kMaxCluster; ++i) {
            t.tcy[i] = tcyCps[i];
          }
          emit(t, sax.pos);
        }
        tcyN = 0;
      } else if (isHeading(sax.name)) {
        capturingTitle = false;
        inHeading = false;
        Atom c{};
        c.kind = AtomKind::ColumnBreak;
        emit(c, sax.pos);
      } else if (isPara(sax.name) || eq(sax.name, "div")) {
        capturingTitle = false;
        uint8_t after = 0;
        const bool closing = lineTop > 0 && lineBox[lineTop - 1].depth == depth;
        if (closing) {
          after = lineBox[lineTop - 1].after;
          --lineTop;
        }
        const bool inLine = lineTop > 0 && isPara(sax.name);
        const bool line = !sink.longText && isPara(sax.name) && textCount < 2000 && (tocTop > 0 || tocPara);
        tocPara = false;
        if (closing || inLine || line) {
          Atom c{};
          c.kind = AtomKind::ColumnBreak;
          if (closing) {
            c.startEm = lineTop > 0 ? lineBox[lineTop - 1].em : 0;
            c.gapAfter = after;
          } else if (line) {
            c.startEm = static_cast<uint8_t>(indent);
          } else {
            c.startEm = lineBox[lineTop - 1].em;
          }
          emit(c, sax.pos);
        } else {
          Atom g{};
          g.kind = AtomKind::Gap;
          emit(g, sax.pos);
        }
      }
      if (skipDepth == 0) {
        if (emphN > 0 && emphFrames[emphN - 1].depth == depth) {
          curEmph = emphFrames[emphN - 1].prev;
          --emphN;
        }
        if (boxN > 0 && boxFrames[boxN - 1].depth == depth) {
          curBox = boxFrames[boxN - 1].prev;
          --boxN;
        }
        if (faceN > 0 && faceFrames[faceN - 1].depth == depth) {
          curScale = faceFrames[faceN - 1].scale;
          curBold = faceFrames[faceN - 1].bold;
          curShade = faceFrames[faceN - 1].shade;
          curRule = faceFrames[faceN - 1].rule;
          curRuleEm = faceFrames[faceN - 1].ruleEm;
          curRuleRoot = faceFrames[faceN - 1].ruleRoot;
          curRuleSides = faceFrames[faceN - 1].ruleSides;
          curFamily = faceFrames[faceN - 1].family;
          --faceN;
        }
        if (tocTop > 0 && tocStack[tocTop - 1] == depth) {
          --tocTop;
        }
        if (depth > 0) {
          --depth;
        }
      }
      if (indentTop > 0) {
        indent = indentStack[--indentTop];
      }
    }
  }

  if (!sink.longText && sawToc && textCount < 2000) {
    res.compactColumns = true;
  }
  if (titleBuf[0]) {
    snprintf(res.title, sizeof(res.title), "%s", titleBuf);
  }
  (void)inBody;
  return true;
}

}  // namespace ts
