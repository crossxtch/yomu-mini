#include "Epub.h"

#include "FaceCss.h"
#include "HtmlSax.h"
#include "ts_picture.h"

#include <Logging.h>
#include <Utf8.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ts {
static_assert(sizeof(EpubBook::Spine) <= 80, "spine slot must leave room for the deflate window");
namespace {

void dirnameOf(char* out, const size_t cap, const char* path) {
  snprintf(out, cap, "%s", path);
  char* slash = strrchr(out, '/');
  if (slash) {
    slash[1] = 0;
  } else {
    out[0] = 0;
  }
}

int hexVal(const char c) {
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

void percentDecodeInPlace(char* s) {
  char* w = s;
  for (char* r = s; *r; ++r) {
    if (r[0] == '%' && hexVal(r[1]) >= 0 && hexVal(r[2]) >= 0) {
      *w++ = static_cast<char>((hexVal(r[1]) << 4) | hexVal(r[2]));
      r += 2;
    } else if (*r == '\\') {
      *w++ = '/';
    } else {
      *w++ = *r;
    }
  }
  *w = 0;
}

void joinPath(char* out, const size_t cap, const char* dir, const char* rel) {
  char decoded[96];
  snprintf(decoded, sizeof(decoded), "%s", rel);
  percentDecodeInPlace(decoded);
  const char* r = decoded;
  while (r[0] == '.' && r[1] == '/') {
    r += 2;
  }
  if (r[0] == '/') {
    snprintf(out, cap, "%s", r + 1);
    return;
  }
  snprintf(out, cap, "%s%s", dir ? dir : "", r);
}

bool isHtmlType(const char* t) {
  if (!t || !t[0]) {
    return true;
  }
  return strstr(t, "html") != nullptr || (strstr(t, "xml") != nullptr && strstr(t, "ncx") == nullptr);
}

bool isImageType(const char* t) { return t && strncmp(t, "image/", 6) == 0; }

bool isSvgType(const char* t) { return t && strstr(t, "svg") != nullptr; }

void joinImagePath(char* out, const size_t cap, const char* dir, const char* rel) {
  char joined[160];
  joinPath(joined, sizeof(joined), dir, rel);
  char* seg[32];
  int n = 0;
  char* p = joined;
  while (*p == '/') {
    ++p;
  }
  while (*p && n < 32) {
    char* slash = strchr(p, '/');
    if (slash) {
      *slash = 0;
    }
    if (strcmp(p, ".") == 0) {
    } else if (strcmp(p, "..") == 0) {
      if (n > 0) {
        --n;
      }
    } else if (p[0]) {
      seg[n++] = p;
    }
    if (!slash) {
      break;
    }
    p = slash + 1;
  }
  size_t used = 0;
  if (cap == 0) {
    return;
  }
  out[0] = 0;
  for (int i = 0; i < n; ++i) {
    const size_t len = strlen(seg[i]);
    if (used + len + 2 >= cap) {
      break;
    }
    if (used) {
      out[used++] = '/';
    }
    memcpy(out + used, seg[i], len);
    used += len;
    out[used] = 0;
  }
}

void stripHash(char* s);

const ZipArchive::Entry* findSpineFile(ZipArchive& zip, const char* opfDir, const char* href) {
  char full[128];
  joinPath(full, sizeof(full), opfDir, href ? href : "");
  const ZipArchive::Entry* ent = zip.find(full);
  if (!ent && href && href[0]) {
    ent = zip.find(href);
  }
  if (!ent && href && href[0]) {
    const char* base = strrchr(href, '/');
    base = base ? base + 1 : href;
    ent = zip.findSuffix(base);
  }
  return ent;
}

const ZipArchive::Entry* findImage(ZipArchive& zip, const char* chapterDir, const char* src) {
  char rel[128];
  snprintf(rel, sizeof(rel), "%s", src ? src : "");
  stripHash(rel);
  if (char* q = strchr(rel, '?')) {
    *q = 0;
  }
  if (rel[0] == 0) {
    return nullptr;
  }
  char full[160];
  joinImagePath(full, sizeof(full), chapterDir, rel);
  const ZipArchive::Entry* ent = zip.find(full);
  if (!ent) {
    ent = zip.find(rel);
  }
  if (!ent) {
    const char* base = strrchr(full[0] ? full : rel, '/');
    base = base ? base + 1 : (full[0] ? full : rel);
    if (base[0]) {
      ent = zip.findSuffix(base);
    }
  }
  return ent;
}

uint16_t addZipImage(ZipArchive& zip, PictureWriter& pics, const ZipArchive::Entry& ent) {
  if (strstr(ent.name, ".svg") || strstr(ent.name, ".SVG")) {
    return 0;
  }
  if (!zip.extractToFile(ent, "/.yomu/work.img")) {
    LOG_ERR("EPUB", "image extract %s", ent.name);
    return 0;
  }
  HalFile hf;
  if (!Storage.openFileForRead("EPUB", "/.yomu/work.img", hf)) {
    return 0;
  }
  hf.probeContiguous();
  uint8_t mag[8]{};
  const int got = hf.read(mag, sizeof(mag));
  if (got < 4 || !hf.seekSet(0)) {
    return 0;
  }
  const bool jpeg = mag[0] == 0xFF && mag[1] == 0xD8;
  const bool png = mag[0] == 0x89 && mag[1] == 'P' && mag[2] == 'N' && mag[3] == 'G';
  const bool gif = mag[0] == 'G' && mag[1] == 'I' && mag[2] == 'F';
  const uint16_t id = jpeg ? pics.addJpeg(hf) : (png || gif ? pics.addPng(hf) : 0);
  if (id == 0) {
    LOG_INF("EPUB", "image skipped %s", ent.name);
  }
  return id;
}

bool isNcxType(const char* t) { return t && strstr(t, "ncx") != nullptr; }

void stripHash(char* s) {
  char* h = strchr(s, '#');
  if (h) {
    *h = 0;
  }
}

bool hrefMatch(const char* spine, const char* tocSrc) {
  char a[96];
  char b[96];
  snprintf(a, sizeof(a), "%s", spine ? spine : "");
  snprintf(b, sizeof(b), "%s", tocSrc ? tocSrc : "");
  percentDecodeInPlace(a);
  percentDecodeInPlace(b);
  stripHash(b);
  if (a[0] == 0 || b[0] == 0) {
    return false;
  }
  if (strcmp(a, b) == 0) {
    return true;
  }
  const char* ba = strrchr(a, '/');
  const char* bb = strrchr(b, '/');
  ba = ba ? ba + 1 : a;
  bb = bb ? bb + 1 : b;
  return strcmp(ba, bb) == 0;
}

void localName(char* out, const size_t cap, const char* path) {
  const char* slash = strrchr(path, '/');
  snprintf(out, cap, "%s", slash ? slash + 1 : path);
}

// A package or TOC bigger than this is inflated onto the card. Holding it
// beside the zip central directory leaves no contiguous block for the spine.
constexpr uint32_t kMemExtractMax = 24576;

struct XmlCursor {
  const char* mem = nullptr;
  size_t memLen = 0;
  HalFile* file = nullptr;
};

int pullHal(void* ctx, char* dst, const int max) {
  if (!ctx || max <= 0) {
    return 0;
  }
  return static_cast<HalFile*>(ctx)->read(dst, static_cast<size_t>(max));
}

bool rewindXml(void* ctx, HtmlSax* sax) {
  auto* cur = static_cast<XmlCursor*>(ctx);
  if (!sax || !cur) {
    return false;
  }
  if (cur->file) {
    if (!cur->file->seekSet(0)) {
      return false;
    }
    sax->bindPull(pullHal, cur->file);
    return true;
  }
  sax->bind(cur->mem ? cur->mem : "", cur->memLen);
  return true;
}

// Memory when the member is small. Otherwise stream to `workPath` and leave
// `file` open at the start. `mem` is malloc'd; the caller frees it.
bool openMember(ZipArchive& zip, const ZipArchive::Entry& ent, const char* workPath, uint8_t*& mem, size_t& memLen,
                HalFile& file, bool& onDisk) {
  mem = nullptr;
  memLen = 0;
  onDisk = false;
  if (ent.uncompSize > 0 && ent.uncompSize <= kMemExtractMax) {
    mem = zip.extract(ent, &memLen);
    if (mem) {
      return true;
    }
  }
  LOG_INF("EPUB", "stream %s uncomp=%lu comp=%lu", ent.name ? ent.name : "?",
          static_cast<unsigned long>(ent.uncompSize), static_cast<unsigned long>(ent.compSize));
  if (!zip.extractToFile(ent, workPath)) {
    return false;
  }
  if (!Storage.openFileForRead("EPUB", workPath, file) || file.fileSize() == 0) {
    LOG_ERR("EPUB", "work open %s", ent.name ? ent.name : "?");
    return false;
  }
  file.probeContiguous();
  onDisk = true;
  return true;
}

constexpr size_t kModeSample = 16384;
constexpr uint32_t kMaxCssBytes = 2u * 1024u * 1024u;
constexpr int kMaxModeClasses = 8;
constexpr int kMaxCssLinks = 8;

struct ModeClasses {
  char name[kMaxModeClasses][32]{};
  int n = 0;
};

bool modeSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool classListed(const ModeClasses& set, const char* s, const size_t n) {
  for (int i = 0; i < set.n; ++i) {
    if (strlen(set.name[i]) == n && memcmp(set.name[i], s, n) == 0) {
      return true;
    }
  }
  return false;
}

void addModeClass(ModeClasses& set, const char* s, const size_t n) {
  if (n == 0 || n >= sizeof(set.name[0]) || set.n >= kMaxModeClasses) {
    return;
  }
  memcpy(set.name[set.n], s, n);
  set.name[set.n][n] = 0;
  ++set.n;
}

void splitModeClasses(ModeClasses& set, const char* s, size_t n) {
  size_t i = 0;
  while (i < n) {
    while (i < n && modeSpace(s[i])) {
      ++i;
    }
    const size_t b = i;
    while (i < n && !modeSpace(s[i])) {
      ++i;
    }
    addModeClass(set, s + b, i - b);
  }
}

bool tagAt(const char* html, const size_t n, const size_t i, const char* tag) {
  const size_t tn = strlen(tag);
  if (i + 1 + tn >= n || html[i] != '<') {
    return false;
  }
  if (memcmp(html + i + 1, tag, tn) != 0) {
    return false;
  }
  const char after = html[i + 1 + tn];
  return after == ' ' || after == '\t' || after == '\n' || after == '\r' || after == '>' || after == '/';
}

bool attrInTag(const char* tag, const size_t n, const char* key, const char** val, size_t* valN) {
  const size_t kn = strlen(key);
  for (size_t i = 0; i + kn + 2 < n; ++i) {
    if ((i > 0 && !modeSpace(tag[i - 1])) || memcmp(tag + i, key, kn) != 0 || tag[i + kn] != '=') {
      continue;
    }
    size_t j = i + kn + 1;
    if (j >= n) {
      return false;
    }
    const char q = tag[j];
    if (q != '"' && q != '\'') {
      continue;
    }
    ++j;
    size_t e = j;
    while (e < n && tag[e] != q) {
      ++e;
    }
    *val = tag + j;
    *valN = e - j;
    return true;
  }
  return false;
}

void classesOnTag(const char* html, const size_t n, const char* tagName, ModeClasses& out) {
  const size_t cap = n < kModeSample ? n : kModeSample;
  for (size_t i = 0; i < cap; ++i) {
    if (!tagAt(html, cap, i, tagName)) {
      continue;
    }
    size_t end = i + 1;
    while (end < cap && html[end] != '>') {
      ++end;
    }
    const char* val = nullptr;
    size_t valN = 0;
    if (attrInTag(html + i, end - i, "class", &val, &valN)) {
      splitModeClasses(out, val, valN);
    }
    return;
  }
}

bool endsWithCss(const char* s) {
  const size_t n = strlen(s);
  return n >= 4 && (memcmp(s + n - 4, ".css", 4) == 0 || memcmp(s + n - 4, ".CSS", 4) == 0);
}

void collectCssHrefs(const char* html, const size_t n, char hrefs[][96], int& count) {
  const size_t cap = n < kModeSample ? n : kModeSample;
  count = 0;
  for (size_t i = 0; i < cap && count < kMaxCssLinks; ++i) {
    if (!tagAt(html, cap, i, "link")) {
      continue;
    }
    size_t end = i + 1;
    while (end < cap && html[end] != '>') {
      ++end;
    }
    const char* val = nullptr;
    size_t valN = 0;
    if (!attrInTag(html + i, end - i, "href", &val, &valN) || valN == 0 || valN >= 96) {
      continue;
    }
    char href[96];
    memcpy(href, val, valN);
    href[valN] = 0;
    if (char* q = strchr(href, '?')) {
      *q = 0;
    }
    if (char* h = strchr(href, '#')) {
      *h = 0;
    }
    if (!endsWithCss(href)) {
      continue;
    }
    snprintf(hrefs[count], 96, "%s", href);
    ++count;
  }
}

const ZipArchive::Entry* findCss(ZipArchive& zip, const char* chapterDir, const char* href) {
  char rel[96];
  snprintf(rel, sizeof(rel), "%s", href ? href : "");
  if (char* q = strchr(rel, '?')) {
    *q = 0;
  }
  if (rel[0] == 0) {
    return nullptr;
  }
  char full[160];
  joinImagePath(full, sizeof(full), chapterDir, rel);
  const ZipArchive::Entry* ent = zip.find(full);
  if (!ent) {
    ent = zip.find(rel);
  }
  if (!ent) {
    const char* base = strrchr(full[0] ? full : rel, '/');
    base = base ? base + 1 : (full[0] ? full : rel);
    if (base[0]) {
      ent = zip.findSuffix(base);
    }
  }
  return ent;
}

bool cssValueMode(const char* s, const size_t n, WritingMode& mode) {
  char buf[32];
  if (n == 0 || n >= sizeof(buf)) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    buf[i] = c;
  }
  buf[n] = 0;
  if (strcmp(buf, "horizontal-tb") == 0 || strcmp(buf, "lr-tb") == 0 || strcmp(buf, "rl-tb") == 0) {
    mode = WritingMode::HorizontalTb;
    return true;
  }
  if (strcmp(buf, "vertical-rl") == 0 || strcmp(buf, "vertical-lr") == 0 || strcmp(buf, "tb-rl") == 0) {
    mode = WritingMode::VerticalRl;
    return true;
  }
  return false;
}

bool classesMatch(const ModeClasses& need, const ModeClasses& have) {
  for (int i = 0; i < need.n; ++i) {
    if (!classListed(have, need.name[i], strlen(need.name[i]))) {
      return false;
    }
  }
  return true;
}

bool selectorPartMatches(const char* s, size_t n, const ModeClasses& htmlC, const ModeClasses& bodyC, int& score) {
  while (n && modeSpace(*s)) {
    ++s;
    --n;
  }
  while (n && modeSpace(s[n - 1])) {
    --n;
  }
  if (n == 0) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (c == '>' || c == '+' || c == '~' || c == '#' || c == '[' || c == ':' || c == '*' || modeSpace(c)) {
      return false;
    }
  }
  enum { Any, Html, Body, Other } el = Any;
  size_t i = 0;
  if (s[0] != '.') {
    while (i < n && s[i] != '.') {
      ++i;
    }
    if (i == 4 && memcmp(s, "html", 4) == 0) {
      el = Html;
    } else if (i == 4 && memcmp(s, "body", 4) == 0) {
      el = Body;
    } else {
      el = Other;
    }
  }
  if (el == Other) {
    return false;
  }
  ModeClasses need{};
  while (i < n) {
    if (s[i] != '.') {
      return false;
    }
    ++i;
    const size_t b = i;
    while (i < n && s[i] != '.') {
      ++i;
    }
    addModeClass(need, s + b, i - b);
    if (need.n == 0 || strlen(need.name[need.n - 1]) != i - b) {
      return false;
    }
  }
  if (el == Any && need.n == 0) {
    return false;
  }
  bool ok = false;
  if (el == Html) {
    ok = classesMatch(need, htmlC);
  } else if (el == Body) {
    ok = classesMatch(need, bodyC);
  } else {
    ok = need.n > 0 && (classesMatch(need, htmlC) || classesMatch(need, bodyC));
  }
  if (!ok) {
    return false;
  }
  score = need.n * 10 + (el == Any ? 0 : 1);
  return score > 0;
}

void applySelector(const char* sel, const size_t n, const WritingMode value, const ModeClasses& htmlC,
                   const ModeClasses& bodyC, int& best, WritingMode& picked, bool& found) {
  size_t start = 0;
  for (size_t i = 0; i <= n; ++i) {
    if (i < n && sel[i] != ',') {
      continue;
    }
    int score = 0;
    if (selectorPartMatches(sel + start, i - start, htmlC, bodyC, score) && score >= best) {
      best = score;
      picked = value;
      found = true;
    }
    start = i + 1;
  }
}


void scanCssMode(char* css, const size_t n, const ModeClasses& htmlC, const ModeClasses& bodyC, int& best,
                 WritingMode& picked, bool& found) {
  blankCssComments(css, n);
  const char key[] = "writing-mode";
  const size_t kn = sizeof(key) - 1;
  for (size_t i = 0; i + kn <= n; ++i) {
    bool name = true;
    for (size_t k = 0; k < kn; ++k) {
      char c = css[i + k];
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
      if (c != key[k]) {
        name = false;
        break;
      }
    }
    if (!name) {
      continue;
    }
    if (i > 0) {
      const unsigned char prev = static_cast<unsigned char>(css[i - 1]);
      if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9') || prev == '_') {
        continue;
      }
    }
    size_t j = i + kn;
    while (j < n && modeSpace(css[j])) {
      ++j;
    }
    if (j >= n || css[j] != ':') {
      continue;
    }
    ++j;
    while (j < n && modeSpace(css[j])) {
      ++j;
    }
    const size_t vb = j;
    while (j < n && css[j] != ';' && css[j] != '}' && css[j] != '!' && !modeSpace(css[j])) {
      ++j;
    }
    WritingMode value = WritingMode::VerticalRl;
    if (!cssValueMode(css + vb, j - vb, value)) {
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
    applySelector(css + selBeg, brace - selBeg, value, htmlC, bodyC, best, picked, found);
  }
}

// Linked stylesheet rule for the html or body class. False when none matches.
bool cssWritingMode(ZipArchive& zip, const char* chapterDir, const char* html, const size_t n, WritingMode& mode) {
  if (!html || n == 0) {
    return false;
  }
  ModeClasses htmlC{};
  ModeClasses bodyC{};
  classesOnTag(html, n, "html", htmlC);
  classesOnTag(html, n, "body", bodyC);
  char hrefs[kMaxCssLinks][96]{};
  int nHref = 0;
  collectCssHrefs(html, n, hrefs, nHref);
  if (nHref == 0) {
    return false;
  }
  int best = 0;
  bool found = false;
  WritingMode picked = mode;
  for (int i = 0; i < nHref; ++i) {
    const ZipArchive::Entry* ent = findCss(zip, chapterDir, hrefs[i]);
    if (!ent || ent->uncompSize == 0 || ent->uncompSize > kMaxCssBytes) {
      continue;
    }
    size_t cssN = 0;
    uint8_t* raw = zip.extract(*ent, &cssN);
    if (!raw) {
      continue;
    }
    if (cssN > 0 && cssN <= kMaxCssBytes) {
      scanCssMode(reinterpret_cast<char*>(raw), cssN, htmlC, bodyC, best, picked, found);
    }
    free(raw);
  }
  if (!found) {
    return false;
  }
  mode = picked;
  return true;
}


void fillFaceRules(ZipArchive& zip, const char* chapterDir, const char* html, const size_t n, FaceRule* rules,
                   uint16_t& count) {
  count = 0;
  if (!rules) {
    return;
  }
  char hrefs[kMaxCssLinks][96]{};
  int nHref = 0;
  if (html && n > 0) {
    collectCssHrefs(html, n, hrefs, nHref);
  }
  for (int i = 0; i < nHref && i < kMaxCssLinks && count < kMaxFaceRules; ++i) {
    const ZipArchive::Entry* ent = findCss(zip, chapterDir, hrefs[i]);
    if (!ent || ent->uncompSize == 0 || ent->uncompSize > kMaxCssBytes) {
      continue;
    }
    size_t cssN = 0;
    uint8_t* raw = zip.extract(*ent, &cssN);
    if (raw && cssN > 0 && cssN <= kMaxCssBytes) {
      char* css = reinterpret_cast<char*>(raw);
      nHref = appendCssImports(css, cssN, hrefs, nHref, kMaxCssLinks);
      scanFaceCss(css, cssN, rules, count);
    }
    free(raw);
  }
  if (html && n > 0) {
    scanStyleTags(html, n, rules, count);
  }
  if (count >= kMaxFaceRules) {
    LOG_INF("EPUB", "face rules full");
  }
}

bool takeWritingMode(const char* xml, size_t n, WritingMode bookMode, WritingMode& chapterMode);

// Tag or inline style wins. Otherwise the linked stylesheet for this chapter.
// A vertical chapter also makes the book vertical, and that sticks.
void resolveChapterMode(ZipArchive& zip, const char* chapterDir, const char* sample, const size_t n,
                        WritingMode& bookMode, WritingMode& chapterMode, bool& named) {
  named = takeWritingMode(sample, n, bookMode, chapterMode);
  if (!named && cssWritingMode(zip, chapterDir, sample, n, chapterMode)) {
    named = true;
  }
  if (named && chapterMode == WritingMode::VerticalRl) {
    bookMode = WritingMode::VerticalRl;
  }
}

// True when the sample names a writing mode. Leaves `chapterMode` alone otherwise.
bool takeWritingMode(const char* xml, const size_t n, const WritingMode bookMode, WritingMode& chapterMode) {
  WritingMode found = bookMode;
  if (!xml || n == 0 || !sniffWritingMode(xml, n, found)) {
    return false;
  }
  chapterMode = found;
  return true;
}

void writeChapterMode(AtomWriter& writer, const WritingMode chapterMode) {
  Atom modeAtom{};
  modeAtom.kind = AtomKind::Mode;
  modeAtom.cp = chapterMode == WritingMode::HorizontalTb ? 1u : 0u;
  writer.write(modeAtom);
}

}  // namespace

bool EpubBook::open(const char* epubPath, const char* atomPath, ProgressFn progress, void* progressCtx,
                    const uint16_t pageW, const uint16_t pageH, const char* picturePath, const uint16_t spineBegin,
                    const uint16_t maxSpines, const bool appendAtoms, const uint32_t atomKeep) {
  close();
  const bool wholeBook = spineBegin == 0 && maxSpines == 0xFFFF && !appendAtoms;
  const unsigned long t0 = millis();
  auto report = [&](uint16_t done, uint16_t total) {
    if (progress) {
      progress(progressCtx, done, total);
    }
    yield();
  };
  report(0, 0);
  if (!zip.open(epubPath)) {
    error = zip.lastError();
    return false;
  }
  LOG_INF("EPUB", "zip %u files %lums", zip.count(), millis() - t0);
  char opfName[96]{};
  if (!parseContainer(opfName, sizeof(opfName))) {
    close();
    return false;
  }
  const ZipArchive::Entry* opfEnt = zip.find(opfName);
  if (!opfEnt) {
    error = "opf missing";
    close();
    return false;
  }
  uint8_t* opf = nullptr;
  size_t opfLen = 0;
  HalFile opfFile;
  bool opfOnDisk = false;
  if (!openMember(zip, *opfEnt, "/.yomu/work.opf", opf, opfLen, opfFile, opfOnDisk)) {
    error = zip.lastError()[0] ? zip.lastError() : "opf read";
    close();
    return false;
  }
  char opfDir[96]{};
  dirnameOf(opfDir, sizeof(opfDir), opfName);
  XmlCursor opfCur;
  opfCur.mem = reinterpret_cast<const char*>(opf);
  opfCur.memLen = opfLen;
  opfCur.file = opfOnDisk ? &opfFile : nullptr;
  const bool okOpf = parseOpf(rewindXml, &opfCur, opfDir);
  free(opf);
  opfFile.close();
  if (!okOpf) {
    close();
    return false;
  }
  if (verticalPreferred) {
    mode = WritingMode::VerticalRl;
  }
  LOG_INF("EPUB", "opf %s spine=%u", opfName, nSpine);

  // Font handle is already released. Drop the zip too so this create is the
  // only open SD file.
  zip.releaseFile();
  AtomWriter writer;
  const bool writerOk = appendAtoms ? writer.openAppend(atomPath, atomKeep) : writer.open(atomPath);
  if (!writerOk) {
    error = "atom file";
    close();
    return false;
  }
  if (!zip.reopenFile()) {
    error = zip.lastError();
    writer.close();
    close();
    return false;
  }

  // A plain character atom is 8 bytes and this xhtml runs about 4 bytes per
  // character, so the sidecar outgrows the chapters. Compare before the
  // 140-second walk. open() already truncated a partial sidecar from the last try.
  uint64_t htmlBytes = 0;
  for (uint16_t i = 0; i < nSpine; ++i) {
    if (items[i].kind != Spine::kHtml || items[i].zipIndex == 0xFFFFu || items[i].zipIndex >= zip.count()) {
      continue;
    }
    htmlBytes += zip.at(items[i].zipIndex).uncompSize;
  }
  uint64_t freeB = 0;
  if (!appendAtoms && Storage.freeBytes(&freeB)) {
    LOG_INF("EPUB", "room free=%lu KB html=%lu KB", static_cast<unsigned long>(freeB / 1024),
            static_cast<unsigned long>(htmlBytes / 1024));
    if (htmlBytes > 0 && freeB < htmlBytes) {
      error = "disk full";
      LOG_ERR("EPUB", "disk full free=%lu KB html=%lu KB", static_cast<unsigned long>(freeB / 1024),
              static_cast<unsigned long>(htmlBytes / 1024));
      writer.close();
      if (atomPath && atomPath[0]) {
        Storage.remove(atomPath);
      }
      close();
      return false;
    }
  }

  PictureWriter pics;
  const bool picsOn = pageW > 0 && pageH > 0 && picturePath && picturePath[0] &&
                      pics.begin(picturePath, pageW, pageH, appendAtoms);
  struct Ctx {
    AtomWriter* w;
    ZipArchive* zip;
    PictureWriter* pics;
    EpubBook* book;
    char chapterDir[128];
    bool ok;
    uint16_t ch;
    uint16_t spine;
  } ctx{&writer, &zip, picsOn ? &pics : nullptr, this, {}, true, 0, 0};
  // The array stays in BSS. The Xteink reader task stack is 8 KB.
  static FaceRule faceRules[kMaxFaceRules];
  memset(faceRules, 0, sizeof faceRules);
  uint16_t nFace = 0;
  AtomSink sink{};
  sink.ctx = &ctx;
  sink.emit = [](void* c, const Atom& a, uint32_t) -> bool {
    auto* x = static_cast<Ctx*>(c);
    if (!x->w->write(a)) {
      x->ok = false;
      return false;
    }
    if (a.kind == AtomKind::Ch || a.kind == AtomKind::Tcy || a.kind == AtomKind::Group) {
      if (x->ch < 0xFFFFu) {
        ++x->ch;
      }
    }
    return true;
  };
  sink.anchor = [](void* c, const char* id) {
    auto* x = static_cast<Ctx*>(c);
    if (x->book && x->w) {
      x->book->noteAnchor(x->spine, id, x->w->position());
    }
  };
  sink.picture = [](void* c, const char* src) -> uint16_t {
    auto* x = static_cast<Ctx*>(c);
    if (!x->pics || !src || !src[0]) {
      return 0;
    }
    const ZipArchive::Entry* ent = findImage(*x->zip, x->chapterDir, src);
    if (!ent) {
      return 0;
    }
    return addZipImage(*x->zip, *x->pics, *ent);
  };

  // A partial slice learns the TOC before inflating, then frees that document
  // inside loadTocDoc. The whole-book path still loads it after the chapters so
  // the NCX does not sit beside the deflate window on a long series.
  bool tocLoaded = false;
  if (!wholeBook) {
    tocLoaded = loadTocDoc(opfDir);
  }

  uint16_t htmlOk = 0;
  uint32_t atomStart = writer.position();
  unsigned long inflateMs = 0;
  unsigned long saxMs = 0;
  const uint16_t begin = spineBegin > nSpine ? nSpine : spineBegin;
  const uint16_t end = (maxSpines == 0xFFFF || static_cast<uint32_t>(begin) + maxSpines > nSpine)
                           ? nSpine
                           : static_cast<uint16_t>(begin + maxSpines);
  sliceEnd = begin;
  for (uint16_t i = begin; i < end; ++i) {
    if (!writer.ok()) {
      break;
    }
    report(i, nSpine);
    items[i].atomOff = writer.position();
    ctx.ch = 0;
    ctx.spine = i;
    sink.forcePlate = items[i].plate != 0;
    ctx.chapterDir[0] = 0;
    const bool indexed = items[i].zipIndex != 0xFFFFu && items[i].zipIndex < zip.count();
    const ZipArchive::Entry* ent = indexed ? &zip.at(items[i].zipIndex) : nullptr;
    if (items[i].kind == Spine::kImage || items[i].kind == Spine::kSvg) {
      uint16_t id = 0;
      if (picsOn && items[i].kind != Spine::kSvg && ent) {
        id = addZipImage(zip, pics, *ent);
      }
      Atom a{};
      a.kind = id != 0 ? AtomKind::Picture : AtomKind::PageBreak;
      a.cp = id != 0 ? (static_cast<uint32_t>(id) | kPictureForcePage) : 0;
      writer.write(a);
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    if (items[i].kind != Spine::kHtml) {
      LOG_INF("EPUB", "skip %s", ent ? ent->name : "?");
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    if (!ent) {
      LOG_ERR("EPUB", "spine missing %u", i);
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    dirnameOf(ctx.chapterDir, sizeof(ctx.chapterDir), ent->name);
    HtmlIRResult ir{};
    const unsigned long tInf = millis();
    bool parsed = false;
    WritingMode chapterMode = mode;
    bool sniffed = false;
    size_t n = 0;
    uint8_t* xml = nullptr;
    HalFile hf;
    bool onDisk = false;
    if (!openMember(zip, *ent, "/.yomu/work.xhtml", xml, n, hf, onDisk)) {
      LOG_ERR("EPUB", "extract %s (%u bytes): %s", ent->name, static_cast<unsigned>(ent->uncompSize), zip.lastError());
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    if (!onDisk && xml && n > 0) {
      inflateMs += millis() - tInf;
      resolveChapterMode(zip, ctx.chapterDir, reinterpret_cast<char*>(xml), n, mode, chapterMode, sniffed);
      fillFaceRules(zip, ctx.chapterDir, reinterpret_cast<char*>(xml), n, faceRules, nFace);
      sink.rules = faceRules;
      sink.nRules = nFace;
      items[i].atomOff = writer.position();
      writeChapterMode(writer, chapterMode);
      const unsigned long tSax = millis();
      htmlToAtoms(reinterpret_cast<char*>(xml), n, sink, &ir);
      saxMs += millis() - tSax;
      parsed = true;
    }
    free(xml);
    if (!parsed && onDisk) {
      inflateMs += millis() - tInf;
      nFace = 0;
      char* sniffBuf = static_cast<char*>(malloc(kModeSample));
      if (sniffBuf) {
        const int got = hf.read(sniffBuf, kModeSample);
        if (got > 0) {
          resolveChapterMode(zip, ctx.chapterDir, sniffBuf, static_cast<size_t>(got), mode, chapterMode, sniffed);
          fillFaceRules(zip, ctx.chapterDir, sniffBuf, static_cast<size_t>(got), faceRules, nFace);
        }
        free(sniffBuf);
      }
      sink.rules = faceRules;
      sink.nRules = nFace;
      hf.seekSet(0);
      items[i].atomOff = writer.position();
      writeChapterMode(writer, chapterMode);
      auto readHf = [](void* ctx, char* dst, int max) -> int {
        return static_cast<HalFile*>(ctx)->read(dst, static_cast<size_t>(max));
      };
      const unsigned long tSax = millis();
      htmlToAtomsPull(readHf, &hf, sink, &ir);
      saxMs += millis() - tSax;
      parsed = true;
    }
    if (!parsed) {
      LOG_ERR("EPUB", "parse %s failed: %s", ent->name, zip.lastError());
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    LOG_INF("EPUB", "ch %u %s %uB chars=%u", i, ent->name, static_cast<unsigned>(ent->uncompSize),
            static_cast<unsigned>(ctx.ch));
    ++htmlOk;
    if (ir.title[0] && items[i].title[0] == 0) {
      snprintf(items[i].title, sizeof(items[i].title), "%s", ir.title);
    }
    items[i].compact = ir.compactColumns ? 1 : 0;
    items[i].hasMode = (sniffed || ir.hasMode) ? 1 : 0;
    items[i].mode = chapterMode;
    items[i].chCount = ctx.ch;
    if (!writer.ok()) {
      break;
    }
    items[i].done = 1;
    sliceEnd = static_cast<uint16_t>(i + 1);
    if (ctx.ch == 0) {
      continue;
    }
    Atom gap{};
    gap.kind = AtomKind::PageBreak;
    writer.write(gap);
  }
  if (!writer.ok()) {
    error = writer.diskFull() ? "disk full" : "atom write";
    const uint32_t at = writer.position();
    writer.close();
    if (appendAtoms && atomKeep >= 4) {
      HalFile torn = Storage.open(atomPath, O_RDWR);
      if (torn) {
        torn.truncate(atomKeep);
        torn.close();
      }
    } else if (atomPath && atomPath[0]) {
      // A failed first slice has no chapter sidecar. Drop it so it does not hold the free space.
      Storage.remove(atomPath);
    }
    LOG_ERR("EPUB", "%s at %lu", error, static_cast<unsigned long>(at));
    close();
    return false;
  }
  if (!tocLoaded && !loadTocDoc(opfDir)) {
    LOG_INF("EPUB", "No NCX/nav TOC");
  }
  for (uint16_t i = begin; i < sliceEnd; ++i) {
    items[i].done = 1;
  }
  bindTocToSpine();
  report(sliceEnd, nSpine);
  const uint32_t atomBytes = writer.position() - atomStart;
  writer.close();
  const uint16_t nPic = pics.count();
  pics.end();
  error = "";
  LOG_INF("EPUB", "'%s' spine=%u/%u html=%u pics=%u atoms=%u infl %lums sax %lums total %lums", bookTitle, sliceEnd,
          nSpine, htmlOk, nPic, static_cast<unsigned>(atomBytes), inflateMs, saxMs, millis() - t0);
  if (wholeBook && ((htmlOk == 0 && nPic == 0) || (atomBytes < 64 && nPic == 0))) {
    error = "no html";
    return false;
  }
  if (!wholeBook && sliceEnd == begin && begin < nSpine) {
    error = "no html";
    return false;
  }
  // The caller reloads the font cmap next. The zip table and the spine are
  // the blocks that used to make that allocation fail.
  free(items);
  items = nullptr;
  zip.close();
  return true;
}

void EpubBook::close() {
  zip.close();
  free(items);
  items = nullptr;
  free(tocs);
  tocs = nullptr;
  free(anchors);
  anchors = nullptr;
  nSpine = 0;
  nToc = 0;
  tocCap = 0;
  nAnchor = 0;
  anchorCap = 0;
  ncxHref[0] = 0;
  navHref[0] = 0;
  bookTitle[0] = 0;
  bookAuthor[0] = 0;
}

bool EpubBook::parseContainer(char* opfName, const size_t cap) {
  const ZipArchive::Entry* e = zip.find("META-INF/container.xml");
  if (!e) {
    error = "no container";
    return false;
  }
  LOG_INF("EPUB", "container %s %u bytes", e->name, static_cast<unsigned>(e->uncompSize));
  size_t n = 0;
  uint8_t* xml = zip.extract(*e, &n);
  if (!xml) {
    error = zip.lastError();
    return false;
  }
  HtmlSax sax;
  sax.bind(reinterpret_cast<char*>(xml), n);
  bool found = false;
  while (sax.next()) {
    if ((sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) && strcmp(sax.name, "rootfile") == 0) {
      if (sax.attr("full-path", opfName, cap) && opfName[0]) {
        found = true;
        break;
      }
    }
  }
  free(xml);
  if (!found) {
    error = "no rootfile";
    return false;
  }
  return true;
}

bool EpubBook::parseOpf(bool (*rewind)(void*, HtmlSax*), void* ctx, const char* opfDir) {
  if (!rewind) {
    error = "opf read";
    return false;
  }
  HtmlSax sax;
  if (!rewind(ctx, &sax)) {
    error = "opf read";
    return false;
  }

  // The spine block is taken first so the id table (kMaxSpine * 40 bytes)
  // sits at the tail. Freeing the ids then leaves one hole big enough for
  // the 32KB deflate window.
  items = static_cast<Spine*>(calloc(kMaxSpine, sizeof(Spine)));
  auto* spineIds = static_cast<char(*)[40]>(calloc(kMaxSpine, 40));
  if (!items || !spineIds) {
    free(items);
    items = nullptr;
    free(spineIds);
    error = "opf oom";
    return false;
  }

  uint16_t nRef = 0;
  bool capped = false;
  bool sawItem = false;
  bool inManifest = false;
  bool inSpine = false;
  bool bookPre = false;
  char tocId[40]{};
  char taking[16]{};
  // 0 unset, 1 pre-paginated, 2 reflowable. Parallel to spineIds.
  auto* itemLay = static_cast<uint8_t*>(calloc(kMaxSpine, 1));
  if (!itemLay) {
    free(items);
    items = nullptr;
    free(spineIds);
    error = "opf oom";
    return false;
  }
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (sax.kind == HtmlSax::Kind::Start) {
        if (strcmp(sax.name, "title") == 0) {
          snprintf(taking, sizeof(taking), "title");
        } else if (strcmp(sax.name, "creator") == 0) {
          snprintf(taking, sizeof(taking), "creator");
        } else {
          taking[0] = 0;
        }
      }
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = true;
      } else if (strcmp(sax.name, "spine") == 0) {
        inSpine = true;
        sax.attr("toc", tocId, sizeof(tocId));
      } else if (inManifest && strcmp(sax.name, "item") == 0) {
        sawItem = true;
      } else if (inSpine && strcmp(sax.name, "itemref") == 0) {
        char id[40]{};
        sax.attr("idref", id, sizeof(id));
        if (id[0] == 0) {
          continue;
        }
        if (nRef < kMaxSpine) {
          char props[160]{};
          if (sax.attr("properties", props, sizeof(props))) {
            if (strstr(props, "rendition:layout-reflowable")) {
              itemLay[nRef] = 2;
            } else if (strstr(props, "rendition:layout-pre-paginated")) {
              itemLay[nRef] = 1;
            }
          }
          snprintf(spineIds[nRef], sizeof(spineIds[nRef]), "%s", id);
          ++nRef;
        } else if (!capped) {
          capped = true;
          LOG_INF("EPUB", "spine capped at %u", static_cast<unsigned>(kMaxSpine));
        }
      } else if (strcmp(sax.name, "meta") == 0) {
        char name[48]{};
        char prop[48]{};
        char content[48]{};
        sax.attr("name", name, sizeof(name));
        sax.attr("property", prop, sizeof(prop));
        sax.attr("content", content, sizeof(content));
        if (strstr(name, "writing-mode") || strstr(prop, "writing-mode") || strcmp(name, "primary-writing-mode") == 0) {
          if (strstr(content, "horizontal") || strstr(content, "lr-tb")) {
            mode = WritingMode::HorizontalTb;
          } else if (strstr(content, "vertical") || strstr(content, "tb-rl")) {
            mode = WritingMode::VerticalRl;
          }
        }
        if (strcmp(prop, "rendition:layout") == 0) {
          if (strstr(content, "reflowable")) {
            bookPre = false;
          } else if (strstr(content, "pre-paginated")) {
            bookPre = true;
          }
        } else if (strcmp(name, "fixed-layout") == 0 && strcmp(content, "true") == 0) {
          bookPre = true;
        }
      }
    } else if (sax.kind == HtmlSax::Kind::End) {
      taking[0] = 0;
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = false;
      } else if (strcmp(sax.name, "spine") == 0) {
        inSpine = false;
      }
    } else if (sax.kind == HtmlSax::Kind::Text) {
      if (strcmp(taking, "title") == 0 && bookTitle[0] == 0) {
        snprintf(bookTitle, sizeof(bookTitle), "%s", sax.text);
      } else if (strcmp(taking, "creator") == 0 && bookAuthor[0] == 0) {
        snprintf(bookAuthor, sizeof(bookAuthor), "%s", sax.text);
      }
    }
  }
  if (!sawItem || nRef == 0) {
    free(items);
    items = nullptr;
    free(spineIds);
    free(itemLay);
    error = "empty spine";
    return false;
  }
  for (uint16_t s = 0; s < nRef; ++s) {
    items[s].zipIndex = 0xFFFFu;
  }
  if (!rewind(ctx, &sax)) {
    free(items);
    items = nullptr;
    free(spineIds);
    free(itemLay);
    error = "opf read";
    return false;
  }

  inManifest = false;
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = true;
      } else if (inManifest && strcmp(sax.name, "item") == 0) {
        char id[40]{};
        char href[96]{};
        char type[40]{};
        sax.attr("id", id, sizeof(id));
        sax.attr("href", href, sizeof(href));
        sax.attr("media-type", type, sizeof(type));
        char props[160]{};
        const bool nav = sax.attr("properties", props, sizeof(props)) && strstr(props, "nav") != nullptr;
        if (href[0] && tocId[0] && strcmp(id, tocId) == 0) {
          snprintf(ncxHref, sizeof(ncxHref), "%s", href);
        } else if (href[0] && isNcxType(type) && ncxHref[0] == 0) {
          snprintf(ncxHref, sizeof(ncxHref), "%s", href);
        }
        if (href[0] && nav && navHref[0] == 0) {
          snprintf(navHref, sizeof(navHref), "%s", href);
        }
        if (id[0] == 0 || href[0] == 0) {
          continue;
        }
        uint8_t kind = Spine::kOther;
        if (isSvgType(type)) {
          kind = Spine::kSvg;
        } else if (isImageType(type)) {
          kind = Spine::kImage;
        } else if (isHtmlType(type)) {
          kind = Spine::kHtml;
        }
        bool wanted = false;
        for (uint16_t s = 0; s < nRef; ++s) {
          if (strcmp(spineIds[s], id) == 0 && items[s].zipIndex == 0xFFFFu) {
            wanted = true;
            break;
          }
        }
        if (!wanted) {
          continue;
        }
        const ZipArchive::Entry* ent = nullptr;
        if (kind == Spine::kImage || kind == Spine::kSvg) {
          char full[128];
          joinPath(full, sizeof(full), opfDir, href);
          ent = findImage(zip, "", full);
          if (!ent) {
            ent = zip.find(href);
          }
        } else {
          ent = findSpineFile(zip, opfDir, href);
        }
        const uint16_t zid = zip.indexOf(ent);
        if (zid == 0xFFFFu) {
          LOG_ERR("EPUB", "spine file %s", href);
          continue;
        }
        for (uint16_t s = 0; s < nRef; ++s) {
          if (strcmp(spineIds[s], id) != 0 || items[s].zipIndex != 0xFFFFu) {
            continue;
          }
          items[s].zipIndex = zid;
          items[s].kind = kind;
          if (itemLay[s] == 0) {
            if (strstr(props, "rendition:layout-reflowable")) {
              itemLay[s] = 2;
            } else if (strstr(props, "rendition:layout-pre-paginated")) {
              itemLay[s] = 1;
            }
          }
        }
      }
    } else if (sax.kind == HtmlSax::Kind::End && strcmp(sax.name, "manifest") == 0) {
      inManifest = false;
    }
  }
  uint16_t live = 0;
  uint16_t preN = 0;
  for (uint16_t i = 0; i < nRef; ++i) {
    if (items[i].zipIndex == 0xFFFFu) {
      continue;
    }
    ++live;
    if (itemLay[i] == 1 || (bookPre && itemLay[i] != 2)) {
      ++preN;
      items[i].plate = 1;
    }
  }
  free(itemLay);
  free(spineIds);
  if (live > 0 && preN == live) {
    free(items);
    items = nullptr;
    error = "fixed layout";
    return false;
  }

  uint16_t w = 0;
  for (uint16_t i = 0; i < nRef; ++i) {
    if (items[i].zipIndex == 0xFFFFu) {
      continue;
    }
    if (w != i) {
      items[w] = items[i];
    }
    ++w;
  }
  nSpine = w;
  if (nSpine == 0) {
    free(items);
    items = nullptr;
    error = "empty spine";
    return false;
  }
  if (nSpine < kMaxSpine) {
    auto* shrunk = static_cast<Spine*>(realloc(items, static_cast<size_t>(nSpine) * sizeof(Spine)));
    if (shrunk) {
      items = shrunk;
    }
  }
  return true;
}

void EpubBook::addToc(const char* title, const char* href, const uint8_t depth) {
  if (!href || href[0] == 0 || nToc >= kMaxSpine) {
    return;
  }
  if (nToc >= tocCap) {
    const uint16_t cap = tocCap == 0 ? 8 : (tocCap * 2 > kMaxSpine ? kMaxSpine : static_cast<uint16_t>(tocCap * 2));
    if (nToc >= cap) {
      return;
    }
    auto* grown = static_cast<TocEntry*>(realloc(tocs, cap * sizeof(TocEntry)));
    if (!grown) {
      return;
    }
    if (cap > tocCap) {
      memset(grown + tocCap, 0, (cap - tocCap) * sizeof(TocEntry));
    }
    tocs = grown;
    tocCap = cap;
  }
  TocEntry& e = tocs[nToc];
  snprintf(e.href, sizeof(e.href), "%s", href);
  percentDecodeInPlace(e.href);
  e.fragment[0] = 0;
  if (char* hash = strchr(e.href, '#')) {
    if (hash[1]) {
      snprintf(e.fragment, sizeof(e.fragment), "%s", hash + 1);
      percentDecodeInPlace(e.fragment);
    }
  }
  stripHash(e.href);
  e.depth = depth;
  if (title && title[0]) {
    snprintf(e.title, sizeof(e.title), "%s", title);
    e.title[utf8SafeTruncateBuffer(e.title, static_cast<int>(strlen(e.title)))] = '\0';
  }
  if (e.title[0] == 0) {
    localName(e.title, sizeof(e.title), e.href);
    char* dot = strrchr(e.title, '.');
    if (dot) {
      *dot = 0;
    }
  }
  e.atomOff = 0xFFFFFFFFu;
  e.spine = 0xFFFFu;
  ++nToc;
}

bool EpubBook::parseNcx(HtmlSax& sax) {
  char label[80]{};
  bool inLabel = false;
  bool takeText = false;
  int pointDepth = 0;
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (strcmp(sax.name, "navpoint") == 0) {
        if (pointDepth < 30) {
          ++pointDepth;
        }
        label[0] = 0;
        inLabel = false;
        takeText = false;
      } else if (strcmp(sax.name, "navlabel") == 0) {
        inLabel = true;
      } else if (strcmp(sax.name, "text") == 0 && inLabel) {
        takeText = true;
      } else if (strcmp(sax.name, "content") == 0) {
        char src[96]{};
        sax.attr("src", src, sizeof(src));
        const uint8_t depth = pointDepth > 0 ? static_cast<uint8_t>(pointDepth - 1) : 0;
        addToc(label, src, depth);
      }
    } else if (sax.kind == HtmlSax::Kind::End) {
      if (strcmp(sax.name, "navpoint") == 0) {
        if (pointDepth > 0) {
          --pointDepth;
        }
      } else if (strcmp(sax.name, "navlabel") == 0) {
        inLabel = false;
        takeText = false;
      } else if (strcmp(sax.name, "text") == 0) {
        takeText = false;
      }
    } else if (sax.kind == HtmlSax::Kind::Text && takeText && sax.text[0]) {
      if (label[0] == 0) {
        snprintf(label, sizeof(label), "%s", sax.text);
      }
    }
  }
  return nToc > 0;
}

bool EpubBook::parseNavDoc(HtmlSax& sax) {
  bool inToc = false;
  char pendingHref[96]{};
  bool takeLink = false;
  int listDepth = 0;
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (strcmp(sax.name, "nav") == 0) {
        inToc = sax.attrContains("type", "toc") || sax.hasClass("toc");
        if (inToc) {
          listDepth = 0;
        }
      } else if (inToc && (strcmp(sax.name, "ol") == 0 || strcmp(sax.name, "ul") == 0)) {
        if (listDepth < 30) {
          ++listDepth;
        }
      } else if (inToc && strcmp(sax.name, "a") == 0) {
        pendingHref[0] = 0;
        sax.attr("href", pendingHref, sizeof(pendingHref));
        takeLink = pendingHref[0] != 0;
      }
    } else if (sax.kind == HtmlSax::Kind::End) {
      if (strcmp(sax.name, "ol") == 0 || strcmp(sax.name, "ul") == 0) {
        if (listDepth > 0) {
          --listDepth;
        }
      }
      if (strcmp(sax.name, "nav") == 0) {
        inToc = false;
        listDepth = 0;
      } else if (strcmp(sax.name, "a") == 0) {
        takeLink = false;
        pendingHref[0] = 0;
      }
    } else if (sax.kind == HtmlSax::Kind::Text && takeLink && sax.text[0] && pendingHref[0]) {
      const uint8_t depth = listDepth > 0 ? static_cast<uint8_t>(listDepth - 1) : 0;
      addToc(sax.text, pendingHref, depth);
      takeLink = false;
      pendingHref[0] = 0;
    }
  }
  return nToc > 0;
}

bool EpubBook::loadTocDoc(const char* opfDir) {
  auto parseRel = [&](const char* rel, const bool ncx) -> bool {
    if (!rel || !rel[0]) {
      return false;
    }
    char full[128];
    joinPath(full, sizeof(full), opfDir, rel);
    const ZipArchive::Entry* ent = zip.find(full);
    if (!ent) {
      ent = zip.find(rel);
    }
    if (!ent) {
      const char* base = strrchr(rel, '/');
      base = base ? base + 1 : rel;
      ent = zip.findSuffix(base);
    }
    if (!ent) {
      return false;
    }
    uint8_t* xml = nullptr;
    size_t n = 0;
    HalFile tocFile;
    bool tocOnDisk = false;
    if (!openMember(zip, *ent, "/.yomu/work.toc", xml, n, tocFile, tocOnDisk)) {
      return false;
    }
    XmlCursor tocCur;
    tocCur.mem = reinterpret_cast<const char*>(xml);
    tocCur.memLen = n;
    tocCur.file = tocOnDisk ? &tocFile : nullptr;
    HtmlSax sax;
    const bool bound = rewindXml(&tocCur, &sax);
    const bool ok = bound && (ncx ? parseNcx(sax) : parseNavDoc(sax));
    free(xml);
    if (!ok) {
      nToc = 0;
    }
    return ok && nToc > 0;
  };
  // EPUB 3: the nav document is the contents list when both nav and NCX exist.
  if (navHref[0]) {
    nToc = 0;
    if (parseRel(navHref, false)) {
      LOG_INF("EPUB", "TOC %s entries=%u", navHref, nToc);
      return true;
    }
    nToc = 0;
  }
  if (ncxHref[0] && parseRel(ncxHref, true)) {
    LOG_INF("EPUB", "TOC %s entries=%u", ncxHref, nToc);
    return true;
  }
  return false;
}

void EpubBook::noteAnchor(const uint16_t spine, const char* id, const uint32_t atomOff) {
  if (!id || !id[0] || spine == 0xFFFFu) {
    return;
  }
  char buf[40];
  snprintf(buf, sizeof(buf), "%s", id);
  if (buf[0] == 0) {
    return;
  }
  for (uint16_t i = 0; i < nAnchor; ++i) {
    if (anchors[i].spine == spine && strcmp(anchors[i].id, buf) == 0) {
      return;
    }
  }
  constexpr uint16_t kMaxAnchors = 160;
  if (nAnchor >= kMaxAnchors) {
    return;
  }
  if (nAnchor >= anchorCap) {
    uint16_t cap = anchorCap == 0 ? 16 : static_cast<uint16_t>(anchorCap * 2);
    if (cap > kMaxAnchors) {
      cap = kMaxAnchors;
    }
    auto* grown = static_cast<AnchorHit*>(realloc(anchors, cap * sizeof(AnchorHit)));
    if (!grown) {
      return;
    }
    if (cap > anchorCap) {
      memset(grown + anchorCap, 0, (cap - anchorCap) * sizeof(AnchorHit));
    }
    anchors = grown;
    anchorCap = cap;
  }
  AnchorHit& hit = anchors[nAnchor++];
  snprintf(hit.id, sizeof(hit.id), "%s", buf);
  hit.spine = spine;
  hit.atomOff = atomOff;
}

uint32_t EpubBook::anchorOffset(const uint16_t spine, const char* id) const {
  if (!id || !id[0]) {
    return 0xFFFFFFFFu;
  }
  for (uint16_t i = 0; i < nAnchor; ++i) {
    if (anchors[i].spine == spine && strcmp(anchors[i].id, id) == 0) {
      return anchors[i].atomOff;
    }
  }
  return 0xFFFFFFFFu;
}

void EpubBook::bindTocToSpine() {
  auto spineName = [&](uint16_t i) -> const char* {
    if (items[i].zipIndex == 0xFFFFu || items[i].zipIndex >= zip.count()) {
      return "";
    }
    return zip.at(items[i].zipIndex).name;
  };
  if (nToc == 0) {
    for (uint16_t i = 0; i < nSpine; ++i) {
      if (!items[i].done || items[i].kind != Spine::kHtml) {
        continue;
      }
      const char* name = spineName(i);
      if (items[i].title[0]) {
        addToc(items[i].title, name);
      } else {
        char fallback[24];
        snprintf(fallback, sizeof(fallback), "Chapter %u", static_cast<unsigned>(nToc + 1));
        addToc(fallback, name);
      }
      if (nToc > 0) {
        tocs[nToc - 1].atomOff = items[i].atomOff;
        tocs[nToc - 1].spine = i;
      }
    }
    LOG_INF("EPUB", "TOC from spine entries=%u", nToc);
    return;
  }
  for (uint16_t t = 0; t < nToc; ++t) {
    char frag[40]{};
    if (tocs[t].fragment[0]) {
      snprintf(frag, sizeof(frag), "%s", tocs[t].fragment);
    } else if (const char* hash = strchr(tocs[t].href, '#')) {
      if (hash[1]) {
        snprintf(frag, sizeof(frag), "%s", hash + 1);
        percentDecodeInPlace(frag);
      }
    }
    for (uint16_t i = 0; i < nSpine; ++i) {
      if (!hrefMatch(spineName(i), tocs[t].href)) {
        continue;
      }
      tocs[t].spine = i;
      if (!items[i].done) {
        tocs[t].atomOff = 0xFFFFFFFFu;
        break;
      }
      if (frag[0]) {
        const uint32_t at = anchorOffset(i, frag);
        if (at != 0xFFFFFFFFu) {
          tocs[t].atomOff = at;
          tocs[t].spine = i;
          break;
        }
      }
      // 角川/青空 style: TOC points at a title-only XHTML; body is the next file.
      // Only walk spines this slice actually wrote. A later file still has chCount 0.
      uint16_t j = i;
      while (j < nSpine && items[j].done && items[j].chCount < 32) {
        ++j;
      }
      if (j >= nSpine || !items[j].done) {
        j = i;
      }
      tocs[t].atomOff = items[j].atomOff;
      tocs[t].spine = j;
      break;
    }
  }
}

}  // namespace ts
