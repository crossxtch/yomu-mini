#include "Mobi.h"

#include "FaceCss.h"
#include "HtmlSax.h"
#include "ts_picture.h"

#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ts {
namespace {

constexpr uint32_t kNotSet = 0xFFFFFFFFu;
constexpr uint32_t kMaxText = 80u * 1024u * 1024u;
constexpr uint32_t kMaxPart = 4u * 1024u * 1024u;
constexpr uint32_t kMaxRec = 8u * 1024u * 1024u;
constexpr uint32_t kMaxCssFlow = 2u * 1024u * 1024u;
constexpr uint16_t kMaxParts = MobiBook::kMaxSpine;
constexpr int kMaxHuffDepth = 12;

uint16_t ru16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t ru32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint32_t pathHash(const char* path) {
  uint32_t h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); p && *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

void cachePaths(const char* book, char* raw, size_t rawN, char* plan, size_t planN) {
  const unsigned long h = pathHash(book);
  snprintf(raw, rawN, "/.yomu/r_%08lx.ml", h);
  snprintf(plan, planN, "/.yomu/p_%08lx.bin", h);
}

bool readAt(HalFile& f, uint32_t off, void* dst, uint32_t n) {
  return n == 0 || (f.seekSet(off) && f.read(dst, n) == static_cast<int>(n));
}

uint8_t* readAlloc(HalFile& f, uint32_t off, uint32_t n) {
  if (n == 0 || n > kMaxRec) {
    return nullptr;
  }
  uint8_t* p = static_cast<uint8_t*>(malloc(n));
  if (!p || !readAt(f, off, p, n)) {
    free(p);
    return nullptr;
  }
  return p;
}

// Backward-encoded trailing size. The high bit sits on the first byte.
uint32_t backwardVwi(const uint8_t* s, uint32_t n, uint32_t end, uint32_t& width) {
  width = 0;
  if (end == 0 || end > n) {
    return 0;
  }
  uint8_t raw[4];
  uint32_t got = 0;
  uint32_t i = end;
  while (i > 0 && got < 4) {
    --i;
    raw[got++] = s[i];
    if (s[i] & 0x80) {
      break;
    }
  }
  width = got;
  uint32_t v = 0;
  for (uint32_t k = got; k > 0; --k) {
    v = (v << 7) | (raw[k - 1] & 0x7f);
  }
  return v;
}

// Extra-flag bits 1..15 sit after the compressed bytes. Bit 0 is the overlap count.
uint32_t trailSize(const uint8_t* s, uint32_t n, uint16_t flags) {
  uint32_t extra = 0;
  for (int bit = 15; bit >= 1; --bit) {
    if ((flags & (1u << bit)) == 0) {
      continue;
    }
    if (extra >= n) {
      return n;
    }
    uint32_t width = 0;
    const uint32_t v = backwardVwi(s, n, n - extra, width);
    if (v == 0 || v > n - extra) {
      return n;
    }
    extra += v;
  }
  if ((flags & 1u) && extra < n) {
    extra += static_cast<uint32_t>(s[n - extra - 1] & 3u) + 1u;
  }
  return extra > n ? n : extra;
}

struct RecTable {
  uint32_t* off = nullptr;
  uint32_t* len = nullptr;
  uint16_t n = 0;
  uint32_t fileBytes = 0;

  ~RecTable() {
    free(off);
    free(len);
  }

  bool load(HalFile& f) {
    uint8_t hdr[78];
    if (!readAt(f, 0, hdr, sizeof hdr)) {
      return false;
    }
    if (memcmp(hdr + 60, "BOOK", 4) != 0 || memcmp(hdr + 64, "MOBI", 4) != 0) {
      return false;
    }
    n = ru16(hdr + 76);
    if (n == 0) {
      return false;
    }
    fileBytes = static_cast<uint32_t>(f.fileSize());
    const uint32_t bytes = static_cast<uint32_t>(n) * 8u;
    uint8_t* tab = readAlloc(f, 78, bytes);
    if (!tab) {
      return false;
    }
    off = static_cast<uint32_t*>(malloc(sizeof(uint32_t) * n));
    len = static_cast<uint32_t*>(malloc(sizeof(uint32_t) * n));
    if (!off || !len) {
      free(tab);
      return false;
    }
    for (uint16_t i = 0; i < n; ++i) {
      off[i] = ru32(tab + static_cast<uint32_t>(i) * 8u);
    }
    free(tab);
    for (uint16_t i = 0; i < n; ++i) {
      const uint32_t next = i + 1 < n ? off[i + 1] : fileBytes;
      if (next < off[i] || off[i] >= fileBytes) {
        len[i] = 0;
      } else {
        len[i] = next - off[i];
      }
    }
    return true;
  }

  uint8_t* read(HalFile& f, uint16_t i) const {
    if (i >= n || len[i] == 0 || len[i] > kMaxRec) {
      return nullptr;
    }
    return readAlloc(f, off[i], len[i]);
  }
};

struct Head {
  uint16_t compression = 0;
  uint16_t encryption = 0;
  uint16_t textCount = 0;
  uint16_t recSize = 4096;
  uint32_t textLen = 0;
  uint32_t headerLen = 0;
  uint32_t mobiType = 0;
  uint32_t encoding = 65001;
  uint32_t version = 0;
  uint32_t image = kNotSet;
  uint32_t huffRec = kNotSet;
  uint32_t huffCount = 0;
  uint32_t fdst = kNotSet;
  uint32_t skel = kNotSet;
  uint32_t frag = kNotSet;
  uint16_t extra = 0;
  uint16_t textStart = 1;
  bool kf8 = false;
  char title[128]{};
  char lang[16]{};
  char writeMode[32]{};
  bool fixed = false;
};

bool exthText(const uint8_t* data, uint32_t n, char* dst, size_t cap) {
  if (!dst || cap == 0) {
    return false;
  }
  dst[0] = 0;
  size_t w = 0;
  for (uint32_t i = 0; i < n && w + 1 < cap; ++i) {
    const uint8_t c = data[i];
    if (c == 0) {
      break;
    }
    if (c < 0x20) {
      continue;
    }
    dst[w++] = static_cast<char>(c);
  }
  dst[w] = 0;
  return w > 0;
}

void parseExth(const uint8_t* rec, uint32_t recN, uint32_t exthAt, Head& h) {
  if (exthAt + 12 > recN || memcmp(rec + exthAt, "EXTH", 4) != 0) {
    return;
  }
  const uint32_t len = ru32(rec + exthAt + 4);
  const uint32_t count = ru32(rec + exthAt + 8);
  if (len < 12 || exthAt + len > recN || count > 512) {
    return;
  }
  uint32_t p = exthAt + 12;
  const uint32_t end = exthAt + len;
  for (uint32_t i = 0; i < count && p + 8 <= end; ++i) {
    const uint32_t tag = ru32(rec + p);
    const uint32_t recLen = ru32(rec + p + 4);
    if (recLen < 8 || p + recLen > end) {
      break;
    }
    const uint8_t* data = rec + p + 8;
    const uint32_t dn = recLen - 8;
    if (tag == 99 && h.title[0] == 0) {
      exthText(data, dn, h.title, sizeof h.title);
    } else if (tag == 121 && dn >= 4) {
      // The boundary record is read by exthBoundary.
    } else if (tag == 122) {
      char tmp[8];
      exthText(data, dn, tmp, sizeof tmp);
      if (strcmp(tmp, "true") == 0) {
        h.fixed = true;
      }
    } else if (tag == 524) {
      exthText(data, dn, h.lang, sizeof h.lang);
    } else if (tag == 525) {
      exthText(data, dn, h.writeMode, sizeof h.writeMode);
    }
    p += recLen;
  }
}

// EXTH 121 is the KF8 boundary record. Parsed apart from the text fields so a
// hybrid file can switch headers before the rest of the fields are trusted.
uint32_t exthBoundary(const uint8_t* rec, uint32_t recN, uint32_t exthAt) {
  if (exthAt + 12 > recN || memcmp(rec + exthAt, "EXTH", 4) != 0) {
    return kNotSet;
  }
  const uint32_t len = ru32(rec + exthAt + 4);
  const uint32_t count = ru32(rec + exthAt + 8);
  if (len < 12 || exthAt + len > recN) {
    return kNotSet;
  }
  uint32_t p = exthAt + 12;
  const uint32_t end = exthAt + len;
  for (uint32_t i = 0; i < count && p + 8 <= end; ++i) {
    const uint32_t tag = ru32(rec + p);
    const uint32_t recLen = ru32(rec + p + 4);
    if (recLen < 8 || p + recLen > end) {
      break;
    }
    if (tag == 121 && recLen >= 12) {
      return ru32(rec + p + 8);
    }
    p += recLen;
  }
  return kNotSet;
}

bool parseHead(const uint8_t* rec, uint32_t recN, Head& h, bool asKf8) {
  if (recN < 16 + 20 || memcmp(rec + 16, "MOBI", 4) != 0) {
    return false;
  }
  h.compression = ru16(rec);
  h.textLen = ru32(rec + 4);
  h.textCount = ru16(rec + 8);
  h.recSize = ru16(rec + 10);
  h.encryption = ru16(rec + 12);
  h.headerLen = ru32(rec + 20);
  h.mobiType = ru32(rec + 24);
  h.encoding = ru32(rec + 28);
  h.version = ru32(rec + 36);
  if (h.headerLen < 20 || 16u + h.headerLen > recN) {
    return false;
  }
  const uint32_t end = 16u + h.headerLen;
  auto field = [&](uint32_t at) -> uint32_t {
    if (at + 4 > end) {
      return kNotSet;
    }
    return ru32(rec + at);
  };
  h.image = field(0x6c);
  h.huffRec = field(0x70);
  h.huffCount = field(0x74);
  if (h.huffCount == kNotSet) {
    h.huffCount = 0;
  }
  if (asKf8 || h.version >= 8 || h.mobiType == 248) {
    h.kf8 = true;
    h.fdst = field(0xc0);
    h.frag = field(0xf8);
    h.skel = field(0xfc);
  } else if (end >= 0xc2) {
    const uint16_t first = ru16(rec + 0xc0);
    if (first >= 1) {
      h.textStart = first;
    }
  }
  if (end >= 0xf4) {
    h.extra = ru16(rec + 0xf2);
  }
  if (field(0x80) & 0x40) {
    parseExth(rec, recN, end, h);
  }
  if (h.title[0] == 0) {
    const uint32_t nameAt = field(0x54);
    const uint32_t nameN = field(0x58);
    if (nameAt != kNotSet && nameN != kNotSet && nameN > 0 && nameAt + nameN <= recN && nameN < sizeof h.title) {
      memcpy(h.title, rec + nameAt, nameN);
      h.title[nameN] = 0;
    }
  }
  if (h.recSize == 0) {
    h.recSize = 4096;
  }
  return h.textCount > 0 && h.textLen > 0 && h.textLen <= kMaxText;
}

bool palmdoc(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap, uint32_t& produced) {
  produced = 0;
  uint32_t i = 0;
  while (i < n && produced < cap) {
    const uint8_t c = in[i++];
    if (c == 0 || (c >= 0x09 && c <= 0x7f)) {
      out[produced++] = c;
    } else if (c <= 0x08) {
      const uint32_t lit = c;
      if (i + lit > n) {
        return false;
      }
      for (uint32_t k = 0; k < lit && produced < cap; ++k) {
        out[produced++] = in[i++];
      }
    } else if (c <= 0xbf) {
      if (i >= n) {
        return false;
      }
      const uint8_t c2 = in[i++];
      const uint32_t packed = (static_cast<uint32_t>(c & 0x3f) << 8) | c2;
      const uint32_t distance = packed >> 3;
      const uint32_t length = (packed & 7u) + 3u;
      if (distance == 0 || distance > produced) {
        return false;
      }
      for (uint32_t k = 0; k < length && produced < cap; ++k) {
        out[produced] = out[produced - distance];
        ++produced;
      }
    } else {
      if (produced + 2 > cap) {
        break;
      }
      out[produced++] = ' ';
      out[produced++] = static_cast<uint8_t>(c ^ 0x80);
    }
  }
  return true;
}

struct Huff {
  struct Code {
    uint8_t len;
    uint8_t term;
    uint32_t maxcode;
  };
  Code dict1[256]{};
  uint32_t minc[33]{};
  uint32_t maxc[33]{};
  // Offset into arena. A pointer would dangle when the arena is reallocated,
  // and a Kindle munmaps that freed block.
  struct Phrase {
    uint32_t off;
    uint16_t n;
    uint8_t lit;
  };
  Phrase* phrase = nullptr;
  uint32_t nphrase = 0;
  uint8_t* arena = nullptr;
  uint32_t arenaN = 0;
  uint32_t arenaCap = 0;

  ~Huff() {
    free(phrase);
    free(arena);
  }

  bool addBytes(const uint8_t* s, uint32_t n, uint32_t& off) {
    off = arenaN;
    if (n == 0) {
      return true;
    }
    if (arenaN + n > arenaCap) {
      uint32_t cap = arenaCap == 0 ? 65536u : arenaCap * 2u;
      while (cap < arenaN + n) {
        cap *= 2u;
      }
      uint8_t* grown = static_cast<uint8_t*>(realloc(arena, cap));
      if (!grown) {
        return false;
      }
      arena = grown;
      arenaCap = cap;
    }
    memcpy(arena + arenaN, s, n);
    arenaN += n;
    return true;
  }

  bool loadHuff(const uint8_t* h, uint32_t n) {
    if (n < 24 || memcmp(h, "HUFF", 4) != 0 || ru32(h + 4) != 0x18) {
      return false;
    }
    const uint32_t off1 = ru32(h + 8);
    const uint32_t off2 = ru32(h + 12);
    if (off1 + 256u * 4u > n || off2 + 64u * 4u > n) {
      return false;
    }
    for (int i = 0; i < 256; ++i) {
      const uint32_t v = ru32(h + off1 + static_cast<uint32_t>(i) * 4u);
      const uint32_t len = v & 0x1f;
      if (len == 0 || len > 32) {
        return false;
      }
      // The field holds the high bits of every terminal code. Codes longer than
      // 8 bits are common in KF8 dictionaries.
      uint32_t maxcode = v >> 8;
      maxcode = ((maxcode + 1u) << (32u - len)) - 1u;
      dict1[i].len = static_cast<uint8_t>(len);
      dict1[i].term = (v & 0x80) ? 1 : 0;
      dict1[i].maxcode = maxcode;
    }
    uint32_t d2[64];
    for (int i = 0; i < 64; ++i) {
      d2[i] = ru32(h + off2 + static_cast<uint32_t>(i) * 4u);
    }
    minc[0] = 0;
    maxc[0] = 0;
    for (int i = 0; i < 32; ++i) {
      const int codelen = i + 1;
      const uint32_t shift = static_cast<uint32_t>(32 - codelen);
      minc[codelen] = d2[i * 2] << shift;
      const uint32_t v = d2[i * 2 + 1];
      maxc[codelen] = shift == 0 ? v : ((v + 1u) << shift) - 1u;
    }
    return true;
  }

  bool loadCdic(const uint8_t* c, uint32_t n) {
    if (n < 16 || memcmp(c, "CDIC", 4) != 0 || ru32(c + 4) != 0x10) {
      return false;
    }
    const uint32_t phrases = ru32(c + 8);
    const uint32_t bits = ru32(c + 12);
    if (bits > 16 || phrases < nphrase) {
      return false;
    }
    uint32_t take = 1u << bits;
    const uint32_t room = phrases - nphrase;
    if (take > room) {
      take = room;
    }
    if (16u + take * 2u > n) {
      return false;
    }
    Phrase* grown = static_cast<Phrase*>(realloc(phrase, sizeof(Phrase) * (nphrase + take)));
    if (!grown) {
      return false;
    }
    phrase = grown;
    for (uint32_t i = 0; i < take; ++i) {
      const uint32_t off = ru16(c + 16 + i * 2);
      if (16u + off + 2u > n) {
        return false;
      }
      const uint16_t blen = ru16(c + 16 + off);
      const uint32_t slen = blen & 0x7fffu;
      if (18u + off + slen > n) {
        return false;
      }
      uint32_t src = 0;
      if (!addBytes(c + 18 + off, slen, src)) {
        return false;
      }
      Phrase& ph = phrase[nphrase++];
      ph.off = src;
      ph.n = static_cast<uint16_t>(slen > 0xffffu ? 0xffffu : slen);
      ph.lit = (blen & 0x8000) ? 1 : 0;
    }
    return true;
  }

  bool unpack(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap, uint32_t& produced, int depth) const {
    produced = 0;
    if (depth > kMaxHuffDepth || nphrase == 0) {
      return false;
    }
    uint8_t padded[8];
    memset(padded, 0, sizeof padded);
    auto be64 = [&](uint32_t pos) -> uint64_t {
      uint8_t b[8];
      for (int i = 0; i < 8; ++i) {
        const uint32_t at = pos + static_cast<uint32_t>(i);
        b[i] = at < n ? in[at] : 0;
      }
      uint64_t v = 0;
      for (int i = 0; i < 8; ++i) {
        v = (v << 8) | b[i];
      }
      return v;
    };
    (void)padded;
    int64_t bits = static_cast<int64_t>(n) * 8;
    uint32_t pos = 0;
    uint64_t x = be64(0);
    int have = 32;
    while (produced < cap) {
      if (have <= 0) {
        pos += 4;
        x = be64(pos);
        have += 32;
      }
      const uint32_t code = static_cast<uint32_t>((x >> have) & 0xffffffffu);
      Code dc = dict1[code >> 24];
      uint32_t codelen = dc.len;
      uint32_t maxcode = dc.maxcode;
      if (!dc.term) {
        while (codelen < 32 && code < minc[codelen]) {
          ++codelen;
        }
        if (codelen > 32) {
          return false;
        }
        maxcode = maxc[codelen];
      }
      if (codelen == 0 || codelen > 32) {
        return false;
      }
      have -= static_cast<int>(codelen);
      bits -= static_cast<int64_t>(codelen);
      if (bits < 0) {
        break;
      }
      const uint32_t r = (maxcode - code) >> (32u - codelen);
      if (r >= nphrase) {
        return false;
      }
      const Phrase& ph = phrase[r];
      if (!arena || ph.off > arenaN || ph.n > arenaN - ph.off) {
        return false;
      }
      const uint8_t* bytes = arena + ph.off;
      if (ph.lit) {
        for (uint16_t i = 0; i < ph.n && produced < cap; ++i) {
          out[produced++] = bytes[i];
        }
      } else {
        uint32_t sub = 0;
        if (!unpack(bytes, ph.n, out + produced, cap - produced, sub, depth + 1)) {
          return false;
        }
        produced += sub;
      }
    }
    return true;
  }
};

// Windows-1252 bytes 0x80..0x9F. Undefined slots stay as the byte itself.
const uint16_t kCp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

bool writeCp(HalFile& out, uint32_t cp) {
  uint8_t b[4];
  int n = 0;
  if (cp < 0x80) {
    b[0] = static_cast<uint8_t>(cp);
    n = 1;
  } else if (cp < 0x800) {
    b[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    b[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    b[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    b[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    b[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    return true;
  }
  return out.write(b, static_cast<size_t>(n)) == static_cast<size_t>(n);
}

bool writeDecoded(HalFile& out, const uint8_t* s, uint32_t n, bool utf8) {
  if (utf8) {
    return n == 0 || out.write(s, n) == n;
  }
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t c = s[i];
    uint32_t cp = c;
    if (c >= 0x80 && c <= 0x9f) {
      cp = kCp1252[c - 0x80];
    }
    if (!writeCp(out, cp)) {
      return false;
    }
  }
  return true;
}

struct IdxEnt {
  char label[48];
  uint32_t t1;
  uint32_t t3;
  uint32_t t6a;
  uint32_t t6b;
  uint8_t has1;
  uint8_t has3;
  uint8_t has6;
};

uint32_t forwardVwi(const uint8_t* s, uint32_t n, uint32_t& i) {
  uint32_t v = 0;
  int guard = 0;
  while (i < n && guard < 5) {
    const uint8_t b = s[i++];
    v = (v << 7) | (b & 0x7f);
    ++guard;
    if (b & 0x80) {
      return v;
    }
  }
  return v;
}

bool loadIndex(HalFile& src, const RecTable& recs, uint32_t idx, IdxEnt* ents, uint16_t cap, uint16_t& nent) {
  nent = 0;
  if (idx == kNotSet || idx >= recs.n) {
    return false;
  }
  uint8_t* main = recs.read(src, static_cast<uint16_t>(idx));
  if (!main || recs.len[idx] < 56 || memcmp(main, "INDX", 4) != 0) {
    free(main);
    return false;
  }
  const uint32_t mainN = recs.len[idx];
  const uint32_t headerLen = ru32(main + 4);
  const uint32_t indexCount = ru32(main + 0x18);
  if (headerLen < 56 || headerLen + 12 > mainN || indexCount == 0 || indexCount > 64) {
    free(main);
    return false;
  }
  if (memcmp(main + headerLen, "TAGX", 4) != 0) {
    free(main);
    return false;
  }
  const uint32_t tagLen = ru32(main + headerLen + 4);
  const uint32_t nctl = ru32(main + headerLen + 8);
  if (tagLen < 12 || headerLen + tagLen > mainN || nctl == 0 || nctl > 4) {
    free(main);
    return false;
  }
  struct Tag {
    uint8_t id;
    uint8_t per;
    uint8_t mask;
    uint8_t end;
  };
  Tag tags[16];
  int ntags = 0;
  for (uint32_t at = 12; at + 4 <= tagLen && ntags < 16; at += 4) {
    Tag& t = tags[ntags++];
    t.id = main[headerLen + at];
    t.per = main[headerLen + at + 1];
    t.mask = main[headerLen + at + 2];
    t.end = main[headerLen + at + 3];
  }
  for (uint32_t r = 0; r < indexCount; ++r) {
    const uint32_t ri = idx + 1 + r;
    if (ri >= recs.n) {
      break;
    }
    uint8_t* data = recs.read(src, static_cast<uint16_t>(ri));
    if (!data || recs.len[ri] < 24 || memcmp(data, "INDX", 4) != 0) {
      free(data);
      continue;
    }
    const uint32_t dn = recs.len[ri];
    const uint32_t idxt = ru32(data + 0x14);
    const uint32_t count = ru32(data + 0x18);
    if (idxt + 4 > dn || count > 4096) {
      free(data);
      continue;
    }
    for (uint32_t e = 0; e < count && nent < cap; ++e) {
      if (idxt + 4 + (e + 1) * 2 > dn) {
        break;
      }
      const uint32_t start = ru16(data + idxt + 4 + e * 2);
      const uint32_t stop = e + 1 < count ? ru16(data + idxt + 4 + (e + 1) * 2) : idxt;
      if (start >= dn || stop > dn || stop < start + 1) {
        continue;
      }
      const uint32_t textN = data[start];
      if (start + 1 + textN + nctl > stop) {
        continue;
      }
      IdxEnt ent{};
      const uint32_t copyN = textN < sizeof(ent.label) - 1 ? textN : sizeof(ent.label) - 1;
      memcpy(ent.label, data + start + 1, copyN);
      ent.label[copyN] = 0;
      const uint32_t ctlAt = start + 1 + textN;
      uint32_t dataAt = ctlAt + nctl;
      uint32_t ctlI = 0;
      uint32_t vals[8];
      for (int t = 0; t < ntags; ++t) {
        if (tags[t].end == 0x01) {
          ++ctlI;
          continue;
        }
        if (ctlI >= nctl) {
          break;
        }
        const uint8_t mask = tags[t].mask;
        uint32_t value = data[ctlAt + ctlI] & mask;
        if (value == 0) {
          continue;
        }
        uint32_t nval = 0;
        uint32_t nbytes = 0;
        bool byBytes = false;
        if (value == mask) {
          int bits = 0;
          for (uint8_t m = mask; m; m = static_cast<uint8_t>(m >> 1)) {
            bits += m & 1;
          }
          if (bits > 1) {
            nbytes = forwardVwi(data, stop, dataAt);
            byBytes = true;
          } else {
            nval = 1;
          }
        } else {
          uint8_t m = mask;
          while ((m & 1) == 0) {
            m = static_cast<uint8_t>(m >> 1);
            value >>= 1;
          }
          nval = value;
        }
        const uint8_t per = tags[t].per == 0 ? 1 : tags[t].per;
        int got = 0;
        if (!byBytes) {
          if (nval > 4) {
            nval = 4;
          }
          for (uint32_t k = 0; k < nval; ++k) {
            for (uint8_t p = 0; p < per && got < 8; ++p) {
              vals[got++] = forwardVwi(data, stop, dataAt);
            }
          }
        } else {
          const uint32_t limit = dataAt + nbytes;
          while (dataAt < limit && dataAt < stop && got < 8) {
            vals[got++] = forwardVwi(data, stop, dataAt);
          }
        }
        if (tags[t].id == 1 && got >= 1) {
          ent.t1 = vals[0];
          ent.has1 = 1;
        } else if (tags[t].id == 3 && got >= 1) {
          ent.t3 = vals[0];
          ent.has3 = 1;
        } else if (tags[t].id == 6 && got >= 2) {
          ent.t6a = vals[0];
          ent.t6b = vals[1];
          ent.has6 = 1;
        }
      }
      ents[nent++] = ent;
    }
    free(data);
  }
  free(main);
  return nent > 0;
}

struct Part {
  uint32_t skelOff = 0;
  uint32_t skelLen = 0;
  uint16_t frag0 = 0;
  uint16_t nfrag = 0;
  char title[80]{};
};

struct Frag {
  uint32_t insert = 0;
  uint32_t len = 0;
};

struct Plan {
  uint32_t srcSize = 0;
  uint32_t rawSize = 0;
  uint32_t image = kNotSet;
  uint16_t npart = 0;
  uint16_t nfrag = 0;
  uint8_t mode = 0;  // 0 vertical, 1 horizontal
  char title[128]{};
  FaceRule* rules = nullptr;
  uint16_t nRules = 0;
  Part* parts = nullptr;
  Frag* frags = nullptr;

  ~Plan() {
    free(rules);
    free(parts);
    free(frags);
  }
};

void rewriteRecindex(uint8_t* s, uint32_t n) {
  const char* fromQ = "recindex=\"";
  const char* toQ = "src=\"mobi:";
  const char* fromS = "recindex='";
  const char* toS = "src='mobi:";
  for (uint32_t i = 0; i + 10 <= n; ++i) {
    if (memcmp(s + i, fromQ, 10) == 0) {
      memcpy(s + i, toQ, 10);
    } else if (memcmp(s + i, fromS, 10) == 0) {
      memcpy(s + i, toS, 10);
    }
  }
}

void scanHeading(const uint8_t* s, uint32_t n, char* dst, size_t cap) {
  if (!dst || cap == 0) {
    return;
  }
  dst[0] = 0;
  for (uint32_t i = 0; i + 3 < n; ++i) {
    if (s[i] != '<') {
      continue;
    }
    char c = static_cast<char>(s[i + 1]);
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != 'h' || s[i + 2] < '1' || s[i + 2] > '6') {
      continue;
    }
    uint32_t j = i + 3;
    while (j < n && s[j] != '>') {
      ++j;
    }
    if (j >= n) {
      return;
    }
    ++j;
    size_t w = 0;
    while (j < n && s[j] != '<' && w + 1 < cap) {
      const uint8_t ch = s[j++];
      if (ch == '\n' || ch == '\r' || ch == '\t') {
        continue;
      }
      dst[w++] = static_cast<char>(ch);
    }
    while (w > 0 && (dst[w - 1] == ' ')) {
      --w;
    }
    dst[w] = 0;
    if (w > 0) {
      return;
    }
  }
}

bool sniffVertical(const char* s, size_t n) {
  WritingMode found = WritingMode::VerticalRl;
  if (!s || n == 0 || !sniffWritingMode(s, n, found)) {
    return false;
  }
  return found == WritingMode::VerticalRl;
}

bool sniffHorizontal(const char* s, size_t n) {
  WritingMode found = WritingMode::VerticalRl;
  if (!s || n == 0 || !sniffWritingMode(s, n, found)) {
    return false;
  }
  return found == WritingMode::HorizontalTb;
}

// A KF8 flow is a stylesheet when it has a rule and is not the book HTML.
bool cssSheet(const char* sheet, const uint32_t n) {
  if (!sheet || n == 0) {
    return false;
  }
  const bool brace = memchr(sheet, '{', n) != nullptr;
  const bool tagged = memchr(sheet, '<', n < 64 ? n : 64) != nullptr;
  return brace && !(tagged && strstr(sheet, "<html") != nullptr);
}

int base32(const char* s) {
  int v = 0;
  if (!s || !s[0]) {
    return -1;
  }
  for (const char* p = s; *p; ++p) {
    char c = *p;
    int d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'A' && c <= 'V') {
      d = c - 'A' + 10;
    } else if (c >= 'a' && c <= 'v') {
      d = c - 'a' + 10;
    } else {
      break;
    }
    v = v * 32 + d;
  }
  return v;
}

void writeChapterMode(AtomWriter& writer, WritingMode chapterMode) {
  Atom modeAtom{};
  modeAtom.kind = AtomKind::Mode;
  modeAtom.cp = chapterMode == WritingMode::HorizontalTb ? 1u : 0u;
  writer.write(modeAtom);
}

bool startsWithI(const char* s, const char* pfx) {
  if (!s || !pfx) {
    return false;
  }
  for (; *pfx; ++s, ++pfx) {
    char a = *s;
    char b = *pfx;
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<char>(b - 'A' + 'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

// PalmDOC text length is the first FDST flow. Later flows stay in the text
// records: the reset sheet, then 囲み罫 and 背景色.
uint32_t rawmlBytes(HalFile& src, RecTable& recs, const Head& head) {
  uint32_t end = head.textLen;
  if (head.fdst == kNotSet || head.fdst >= recs.n) {
    return end;
  }
  uint8_t* fd = recs.read(src, static_cast<uint16_t>(head.fdst));
  if (!fd || recs.len[head.fdst] < 20 || memcmp(fd, "FDST", 4) != 0) {
    free(fd);
    return end;
  }
  const uint32_t nsec = ru32(fd + 8);
  if (nsec >= 1 && nsec <= 64 && 12u + nsec * 8u <= recs.len[head.fdst]) {
    const uint32_t last = ru32(fd + 12 + (nsec - 1) * 8 + 4);
    if (last > end && last <= kMaxText) {
      end = last;
    }
  }
  free(fd);
  return end;
}

}  // namespace

bool MobiBook::open(const char* path, const char* atomPath, ProgressFn progress, void* progressCtx, const uint16_t pageW,
                    const uint16_t pageH, const char* picturePath, const uint16_t spineBegin, const uint16_t maxSpines,
                    const bool appendAtoms, const uint32_t atomKeep) {
  close();
  error = "mobi";
  auto report = [&](uint16_t done, uint16_t total) {
    if (progress) {
      progress(progressCtx, done, total);
    }
    yield();
  };
  report(0, 0);
  if (!path || !path[0] || !atomPath || !atomPath[0]) {
    return false;
  }

  char rawPath[64];
  char planPath[64];
  cachePaths(path, rawPath, sizeof rawPath, planPath, sizeof planPath);
  HalFile src;
  if (!Storage.openFileForRead("MOBI", path, src)) {
    error = "mobi";
    return false;
  }
  const uint32_t srcSize = static_cast<uint32_t>(src.fileSize());
  RecTable recs;
  if (!recs.load(src)) {
    error = "mobi";
    return false;
  }

  uint8_t* rec0 = recs.read(src, 0);
  if (!rec0) {
    error = "mobi";
    return false;
  }
  Head head;
  if (!parseHead(rec0, recs.len[0], head, false)) {
    free(rec0);
    error = "mobi";
    return false;
  }
  const uint32_t exthAt = 16u + head.headerLen;
  const uint32_t boundary = (head.headerLen >= 20) ? exthBoundary(rec0, recs.len[0], exthAt) : kNotSet;
  uint16_t headerRec = 0;
  if (boundary != kNotSet && boundary + 1 < recs.n) {
    uint8_t* mark = recs.read(src, static_cast<uint16_t>(boundary));
    const bool isBoundary = mark && recs.len[boundary] >= 8 && memcmp(mark, "BOUNDARY", 8) == 0;
    free(mark);
    if (isBoundary) {
      uint8_t* kf = recs.read(src, static_cast<uint16_t>(boundary + 1));
      Head kfHead;
      if (kf && parseHead(kf, recs.len[boundary + 1], kfHead, true)) {
        if (kfHead.title[0] == 0 && head.title[0]) {
          snprintf(kfHead.title, sizeof kfHead.title, "%s", head.title);
        }
        if (kfHead.lang[0] == 0 && head.lang[0]) {
          snprintf(kfHead.lang, sizeof kfHead.lang, "%s", head.lang);
        }
        head = kfHead;
        headerRec = static_cast<uint16_t>(boundary + 1);
        head.textStart = static_cast<uint16_t>(headerRec + 1);
      }
      free(kf);
    }
  } else if (head.version >= 8 || head.mobiType == 248) {
    Head again;
    if (parseHead(rec0, recs.len[0], again, true)) {
      head = again;
      head.textStart = 1;
    }
  }
  free(rec0);

  if (head.encryption != 0) {
    error = "drm";
    return false;
  }
  if (head.fixed) {
    error = "fixed layout";
    return false;
  }
  if (head.encoding == 65002) {
    error = "encoding";
    return false;
  }
  if (head.mobiType == 4 || head.mobiType == 513 || head.mobiType == 514) {
    error = "mobi";
    return false;
  }
  if (head.textStart == 0 || head.textStart >= recs.n ||
      static_cast<uint32_t>(head.textStart) + head.textCount > recs.n) {
    error = "mobi";
    return false;
  }
  const bool utf8 = head.encoding == 65001 || head.kf8;

  // Rebuild the unpacked markup when the book file changed.
  bool fresh = false;
  {
    HalFile planFile;
    uint8_t mag[8];
    if (Storage.openFileForRead("MOBI", planPath, planFile) && planFile.read(mag, 8) == 8 &&
        memcmp(mag, kPlanMagic, 4) == 0) {
      const uint32_t stored = ru32(mag + 4);
      fresh = stored == srcSize && Storage.exists(rawPath);
    }
  }

  if (!fresh) {
    uint64_t freeB = 0;
    // textLen stops at flow 0. The stylesheet flows are past it, still in the
    // text records, so the unpack has to reach the last FDST offset.
    const uint32_t rawNeed = rawmlBytes(src, recs, head);
    if (Storage.freeBytes(&freeB) && freeB < rawNeed) {
      error = "disk full";
      return false;
    }
    Huff huff;
    if (head.compression == 17480) {
      if (head.huffRec == kNotSet || head.huffCount < 2 || head.huffRec >= recs.n) {
        error = "mobi";
        return false;
      }
      uint8_t* hr = recs.read(src, static_cast<uint16_t>(head.huffRec));
      if (!hr || !huff.loadHuff(hr, recs.len[head.huffRec])) {
        free(hr);
        error = "mobi";
        return false;
      }
      free(hr);
      for (uint32_t i = 1; i < head.huffCount && head.huffRec + i < recs.n; ++i) {
        uint8_t* cd = recs.read(src, static_cast<uint16_t>(head.huffRec + i));
        if (!cd || recs.len[head.huffRec + i] < 4 || memcmp(cd, "CDIC", 4) != 0) {
          free(cd);
          break;
        }
        const bool ok = huff.loadCdic(cd, recs.len[head.huffRec + i]);
        free(cd);
        if (!ok) {
          error = "mobi";
          return false;
        }
      }
    } else if (head.compression != 1 && head.compression != 2) {
      error = "mobi";
      return false;
    }

    HalFile raw;
    if (!Storage.openFileForWrite("MOBI", rawPath, raw)) {
      error = "mobi";
      return false;
    }
    uint32_t written = 0;
    constexpr uint32_t cap = 65536;
    uint8_t* block = static_cast<uint8_t*>(malloc(cap));
    if (!block) {
      error = "mobi";
      return false;
    }
    bool unpackOk = true;
    for (uint16_t t = 0; t < head.textCount && written < rawNeed; ++t) {
      const uint16_t ri = static_cast<uint16_t>(head.textStart + t);
      uint8_t* comp = recs.read(src, ri);
      if (!comp) {
        unpackOk = false;
        break;
      }
      uint32_t n = recs.len[ri];
      const uint32_t trail = trailSize(comp, n, head.extra);
      if (trail < n) {
        n -= trail;
      } else {
        n = 0;
      }
      uint32_t produced = 0;
      const uint32_t room = rawNeed - written;
      const uint32_t outCap = room < cap ? room : cap;
      if (head.compression == 1) {
        produced = n < outCap ? n : outCap;
        if (produced > cap) {
          produced = cap;
        }
        memcpy(block, comp, produced);
      } else if (head.compression == 2) {
        unpackOk = palmdoc(comp, n, block, outCap, produced);
      } else {
        unpackOk = huff.unpack(comp, n, block, outCap, produced, 0);
      }
      free(comp);
      if (!unpackOk) {
        break;
      }
      if (produced > room) {
        produced = room;
      }
      if (!writeDecoded(raw, block, produced, utf8)) {
        unpackOk = false;
        error = "disk full";
        break;
      }
      written += produced;
      yield();
    }
    free(block);
    raw.close();
    if (!unpackOk || written == 0) {
      Storage.remove(rawPath);
      if (strcmp(error, "disk full") != 0) {
        error = "mobi";
      }
      return false;
    }

    // Flows, skeleton, and fragments. A book with no skeleton is one HTML part.
    uint32_t flow0 = 0;
    uint32_t flow0End = written;
    uint32_t flowStart[64]{};
    uint32_t flowEnd[64]{};
    uint32_t nflow = 0;
    if (head.fdst != kNotSet && head.fdst < recs.n) {
      uint8_t* fd = recs.read(src, static_cast<uint16_t>(head.fdst));
      if (fd && recs.len[head.fdst] >= 20 && memcmp(fd, "FDST", 4) == 0) {
        const uint32_t nsec = ru32(fd + 8);
        if (nsec >= 1 && nsec <= 64 && 12u + nsec * 8u <= recs.len[head.fdst]) {
          nflow = nsec;
          for (uint32_t i = 0; i < nflow; ++i) {
            flowStart[i] = ru32(fd + 12 + i * 8);
            flowEnd[i] = ru32(fd + 16 + i * 8);
          }
          flow0 = flowStart[0];
          flow0End = nflow >= 2 ? flowStart[1] : flowEnd[0];
          if (flow0End > written || flow0End < flow0) {
            flow0End = written;
          }
        }
      }
      free(fd);
    }
    if (flow0 > written) {
      flow0 = 0;
      flow0End = written;
    }

    IdxEnt* ents = static_cast<IdxEnt*>(calloc(kMaxParts, sizeof(IdxEnt)));
    Part* parts = static_cast<Part*>(calloc(kMaxParts, sizeof(Part)));
    Frag* frags = static_cast<Frag*>(calloc(kMaxParts * 4u, sizeof(Frag)));
    if (!ents || !parts || !frags) {
      free(ents);
      free(parts);
      free(frags);
      error = "mobi";
      return false;
    }
    uint16_t npart = 0;
    uint16_t nfrag = 0;
    uint16_t nskel = 0;
    const uint32_t fragCap = kMaxParts * 4u;
    if (head.skel != kNotSet && loadIndex(src, recs, head.skel, ents, kMaxParts, nskel)) {
      IdxEnt* fents = static_cast<IdxEnt*>(calloc(fragCap, sizeof(IdxEnt)));
      uint16_t nf = 0;
      if (fents && head.frag != kNotSet) {
        loadIndex(src, recs, head.frag, fents, static_cast<uint16_t>(fragCap), nf);
      }
      uint16_t fragI = 0;
      for (uint16_t s = 0; s < nskel && npart < kMaxParts; ++s) {
        if (!ents[s].has6) {
          continue;
        }
        Part& part = parts[npart];
        part.skelOff = flow0 + ents[s].t6a;
        part.skelLen = ents[s].t6b;
        part.frag0 = nfrag;
        const uint16_t want = ents[s].has1 ? static_cast<uint16_t>(ents[s].t1) : 0;
        for (uint16_t k = 0; k < want && fragI < nf && nfrag < fragCap; ++k, ++fragI) {
          Frag& fr = frags[nfrag++];
          fr.insert = flow0 + static_cast<uint32_t>(strtoul(fents[fragI].label, nullptr, 10));
          fr.len = fents[fragI].has6 ? fents[fragI].t6b : 0;
          ++part.nfrag;
        }
        if (part.skelOff < written && part.skelLen > 0 && part.skelOff + part.skelLen <= written + 8) {
          ++npart;
        } else {
          nfrag = part.frag0;
        }
      }
      free(fents);
    }
    if (npart == 0) {
      parts[0].skelOff = flow0;
      parts[0].skelLen = flow0End > flow0 ? flow0End - flow0 : 0;
      npart = parts[0].skelLen > 0 ? 1 : 0;
    }

    // Flow 0 is the book. Later flows are stylesheets. Each one is scanned on
    // its own, so a sheet larger than 64KB still contributes its face rules.
    uint8_t bookMode = 0;
    bool sawVertical = false;
    bool sawHorizontal = false;
    auto* cssRules = static_cast<FaceRule*>(calloc(kMaxFaceRules, sizeof(FaceRule)));
    uint16_t nCss = 0;
    if (nflow >= 2 && cssRules) {
      HalFile rawIn;
      if (Storage.openFileForRead("MOBI", rawPath, rawIn)) {
        for (uint32_t fi = 1; fi < nflow && nCss < kMaxFaceRules; ++fi) {
          const uint32_t a = flowStart[fi];
          const uint32_t b = flowEnd[fi];
          if (b <= a || a >= written || b > written) {
            continue;
          }
          const uint32_t n = b - a;
          if (n > kMaxCssFlow) {
            LOG_INF("MOBI", "css flow %u skipped %u", fi, n);
            continue;
          }
          char* sheet = static_cast<char*>(malloc(n + 1));
          if (!sheet) {
            continue;
          }
          if (!rawIn.seekSet(a) || rawIn.read(sheet, n) != static_cast<int>(n)) {
            free(sheet);
            continue;
          }
          sheet[n] = 0;
          if (!cssSheet(sheet, n)) {
            free(sheet);
            continue;
          }
          if (sniffVertical(sheet, n)) {
            sawVertical = true;
          } else if (sniffHorizontal(sheet, n)) {
            sawHorizontal = true;
          }
          scanFaceCss(sheet, n, cssRules, nCss);
          free(sheet);
        }
      }
    }
    LOG_INF("MOBI", "face rules %u", static_cast<unsigned>(nCss));
    if (strstr(head.writeMode, "vertical") || strstr(head.writeMode, "tb-rl")) {
      sawVertical = true;
    } else if (strstr(head.writeMode, "horizontal") || strstr(head.writeMode, "lr-tb")) {
      sawHorizontal = true;
    }
    HalFile rawIn;
    Storage.openFileForRead("MOBI", rawPath, rawIn);
    for (uint16_t i = 0; i < npart; ++i) {
      const uint32_t totalFr = [&]() {
        uint32_t t = parts[i].skelLen;
        for (uint16_t k = 0; k < parts[i].nfrag; ++k) {
          t += frags[parts[i].frag0 + k].len;
        }
        return t;
      }();
      if (!rawIn || totalFr == 0 || totalFr > kMaxPart) {
        continue;
      }
      uint8_t* buf = static_cast<uint8_t*>(malloc(totalFr + 1));
      if (!buf) {
        continue;
      }
      uint32_t cur = 0;
      if (rawIn.seekSet(parts[i].skelOff) && parts[i].skelLen > 0) {
        const int got = rawIn.read(buf, parts[i].skelLen);
        cur = got > 0 ? static_cast<uint32_t>(got) : 0;
      }
      uint32_t base = parts[i].skelOff + parts[i].skelLen;
      for (uint16_t k = 0; k < parts[i].nfrag && cur < totalFr; ++k) {
        const Frag& fr = frags[parts[i].frag0 + k];
        uint32_t ins = fr.insert >= parts[i].skelOff ? fr.insert - parts[i].skelOff : cur;
        if (ins > cur) {
          ins = cur;
        }
        if (fr.len > totalFr - cur) {
          break;
        }
        memmove(buf + ins + fr.len, buf + ins, cur - ins);
        if (!rawIn.seekSet(base) || rawIn.read(buf + ins, fr.len) != static_cast<int>(fr.len)) {
          memset(buf + ins, ' ', fr.len);
        }
        cur += fr.len;
        base += fr.len;
      }
      buf[cur] = 0;
      scanHeading(buf, cur, parts[i].title, sizeof parts[i].title);
      if (sniffVertical(reinterpret_cast<char*>(buf), cur)) {
        sawVertical = true;
      } else if (sniffHorizontal(reinterpret_cast<char*>(buf), cur)) {
        sawHorizontal = true;
      }
      free(buf);
    }
    if (sawVertical) {
      bookMode = 0;
    } else if (sawHorizontal || startsWithI(head.lang, "en")) {
      bookMode = 1;
    }

    HalFile out;
    if (npart == 0 || !Storage.openFileForWrite("MOBI", planPath, out)) {
      free(ents);
      free(parts);
      free(frags);
      free(cssRules);
      error = "mobi";
      return false;
    }
    uint8_t mag[8] = {kPlanMagic[0], kPlanMagic[1], kPlanMagic[2], kPlanMagic[3], 0, 0, 0, 0};
    mag[4] = static_cast<uint8_t>(srcSize >> 24);
    mag[5] = static_cast<uint8_t>(srcSize >> 16);
    mag[6] = static_cast<uint8_t>(srcSize >> 8);
    mag[7] = static_cast<uint8_t>(srcSize);
    auto wu32 = [&](uint32_t v) {
      uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                      static_cast<uint8_t>(v)};
      return out.write(b, 4) == 4;
    };
    auto wu16 = [&](uint16_t v) {
      uint8_t b[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
      return out.write(b, 2) == 2;
    };
    bool wok = out.write(mag, 8) == 8 && wu32(written) && wu32(head.image) && wu16(npart) && wu16(nfrag) &&
               out.write(&bookMode, 1) == 1;
    const uint8_t titleN = static_cast<uint8_t>(strlen(head.title) > 120 ? 120 : strlen(head.title));
    wok = wok && out.write(&titleN, 1) == 1 && (titleN == 0 || out.write(head.title, titleN) == titleN);
    wok = wok && wu16(nCss);
    for (uint16_t r = 0; wok && cssRules && r < nCss; ++r) {
      const FaceRule& rule = cssRules[r];
      const uint8_t tail[12] = {rule.weight,  rule.pct,    rule.root,     rule.emphasis, rule.box,   rule.shade,
                               rule.rulePx,  rule.ruleEm, rule.ruleRoot, rule.ruleSides, rule.family, rule.mode};
      wok = out.write(rule.tag, sizeof rule.tag) == sizeof rule.tag &&
            out.write(rule.cls, sizeof rule.cls) == sizeof rule.cls && out.write(tail, sizeof tail) == sizeof tail;
    }
    for (uint16_t i = 0; wok && i < npart; ++i) {
      const uint8_t tn = static_cast<uint8_t>(strlen(parts[i].title) > 79 ? 79 : strlen(parts[i].title));
      wok = wu32(parts[i].skelOff) && wu32(parts[i].skelLen) && wu16(parts[i].nfrag) && out.write(&tn, 1) == 1 &&
            (tn == 0 || out.write(parts[i].title, tn) == tn);
      for (uint16_t k = 0; wok && k < parts[i].nfrag; ++k) {
        const Frag& fr = frags[parts[i].frag0 + k];
        wok = wu32(fr.insert) && wu32(fr.len);
      }
    }
    free(ents);
    free(parts);
    free(frags);
    free(cssRules);
    out.close();
    if (!wok) {
      Storage.remove(planPath);
      Storage.remove(rawPath);
      error = "mobi";
      return false;
    }
    LOG_INF("MOBI", "unpacked %u bytes parts=%u", written, npart);
  }

  Plan plan;
  {
    HalFile pf;
    if (!Storage.openFileForRead("MOBI", planPath, pf)) {
      error = "mobi";
      return false;
    }
    uint8_t mag[8];
    if (pf.read(mag, 8) != 8 || memcmp(mag, kPlanMagic, 4) != 0 || ru32(mag + 4) != srcSize) {
      error = "mobi";
      return false;
    }
    uint8_t b4[4];
    uint8_t b2[2];
    if (pf.read(b4, 4) != 4) {
      error = "mobi";
      return false;
    }
    plan.rawSize = ru32(b4);
    if (pf.read(b4, 4) != 4) {
      error = "mobi";
      return false;
    }
    plan.image = ru32(b4);
    if (pf.read(b2, 2) != 2) {
      error = "mobi";
      return false;
    }
    plan.npart = ru16(b2);
    if (pf.read(b2, 2) != 2) {
      error = "mobi";
      return false;
    }
    plan.nfrag = ru16(b2);
    uint8_t modeB = 0;
    uint8_t titleN = 0;
    if (pf.read(&modeB, 1) != 1 || pf.read(&titleN, 1) != 1 || titleN > 120) {
      error = "mobi";
      return false;
    }
    plan.mode = modeB;
    if (titleN > 0 && pf.read(plan.title, titleN) != titleN) {
      error = "mobi";
      return false;
    }
    plan.title[titleN] = 0;
    if (pf.read(b2, 2) != 2) {
      error = "mobi";
      return false;
    }
    plan.nRules = ru16(b2);
    if (plan.nRules > kMaxFaceRules) {
      error = "mobi";
      return false;
    }
    if (plan.nRules > 0) {
      plan.rules = static_cast<FaceRule*>(calloc(plan.nRules, sizeof(FaceRule)));
      if (!plan.rules) {
        error = "mobi";
        return false;
      }
      for (uint16_t r = 0; r < plan.nRules; ++r) {
        FaceRule& rule = plan.rules[r];
        uint8_t tail[12];
        if (pf.read(rule.tag, sizeof rule.tag) != static_cast<int>(sizeof rule.tag) ||
            pf.read(rule.cls, sizeof rule.cls) != static_cast<int>(sizeof rule.cls) ||
            pf.read(tail, sizeof tail) != static_cast<int>(sizeof tail)) {
          error = "mobi";
          return false;
        }
        rule.weight = tail[0];
        rule.pct = tail[1];
        rule.root = tail[2];
        rule.emphasis = tail[3];
        rule.box = tail[4];
        rule.shade = tail[5];
        rule.rulePx = tail[6];
        rule.ruleEm = tail[7];
        rule.ruleRoot = tail[8];
        rule.ruleSides = tail[9];
        rule.family = tail[10];
        rule.mode = tail[11];
      }
    }
    if (plan.npart == 0 || plan.npart > kMaxParts) {
      error = "no html";
      return false;
    }
    plan.parts = static_cast<Part*>(calloc(plan.npart, sizeof(Part)));
    if (plan.nfrag > 0) {
      plan.frags = static_cast<Frag*>(calloc(plan.nfrag, sizeof(Frag)));
    }
    if (!plan.parts || (plan.nfrag > 0 && !plan.frags)) {
      error = "mobi";
      return false;
    }
    uint16_t fragFill = 0;
    for (uint16_t i = 0; i < plan.npart; ++i) {
      uint8_t tn = 0;
      if (pf.read(b4, 4) != 4) {
        error = "mobi";
        return false;
      }
      plan.parts[i].skelOff = ru32(b4);
      if (pf.read(b4, 4) != 4) {
        error = "mobi";
        return false;
      }
      plan.parts[i].skelLen = ru32(b4);
      if (pf.read(b2, 2) != 2 || pf.read(&tn, 1) != 1 || tn > 79) {
        error = "mobi";
        return false;
      }
      plan.parts[i].nfrag = ru16(b2);
      plan.parts[i].frag0 = fragFill;
      if (tn > 0 && pf.read(plan.parts[i].title, tn) != tn) {
        error = "mobi";
        return false;
      }
      plan.parts[i].title[tn] = 0;
      for (uint16_t k = 0; k < plan.parts[i].nfrag; ++k) {
        if (fragFill >= plan.nfrag || pf.read(b4, 4) != 4) {
          error = "mobi";
          return false;
        }
        plan.frags[fragFill].insert = ru32(b4);
        if (pf.read(b4, 4) != 4) {
          error = "mobi";
          return false;
        }
        plan.frags[fragFill].len = ru32(b4);
        ++fragFill;
      }
    }
    plan.srcSize = srcSize;
  }

  if (plan.title[0]) {
    snprintf(bookTitle, sizeof bookTitle, "%s", plan.title);
  }
  mode = plan.mode == 1 ? WritingMode::HorizontalTb : WritingMode::VerticalRl;
  if (verticalPreferred) {
    mode = WritingMode::VerticalRl;
  }
  nSpine = plan.npart;
  nToc = 0;
  tocs = static_cast<TocEntry*>(calloc(nSpine, sizeof(TocEntry)));
  if (!tocs) {
    error = "mobi";
    return false;
  }
  for (uint16_t i = 0; i < nSpine; ++i) {
    if (plan.parts[i].title[0] == 0) {
      continue;
    }
    TocEntry& t = tocs[nToc++];
    snprintf(t.title, sizeof t.title, "%s", plan.parts[i].title);
    t.spine = i;
    t.atomOff = 0xFFFFFFFFu;
  }

  AtomWriter writer;
  const bool writerOk = appendAtoms ? writer.openAppend(atomPath, atomKeep) : writer.open(atomPath);
  if (!writerOk) {
    error = "atom file";
    return false;
  }
  PictureWriter pics;
  const bool picsOn = pageW > 0 && pageH > 0 && picturePath && picturePath[0] &&
                      pics.begin(picturePath, pageW, pageH, appendAtoms);

  struct Ctx {
    AtomWriter* w;
    PictureWriter* pics;
    HalFile* book;
    const RecTable* recs;
    uint32_t image;
    bool ok;
  } ctx{&writer, picsOn ? &pics : nullptr, &src, &recs, plan.image, true};

  static FaceRule faceRules[kMaxFaceRules];
  uint16_t nFace = 0;
  if (plan.rules && plan.nRules > 0) {
    nFace = plan.nRules > kMaxFaceRules ? kMaxFaceRules : plan.nRules;
    memcpy(faceRules, plan.rules, static_cast<size_t>(nFace) * sizeof(FaceRule));
  }
  AtomSink sink{};
  sink.ctx = &ctx;
  sink.rules = faceRules;
  sink.nRules = nFace;
  sink.emit = [](void* c, const Atom& a, uint32_t) -> bool {
    auto* x = static_cast<Ctx*>(c);
    if (!x->w->write(a)) {
      x->ok = false;
      return false;
    }
    return true;
  };
  sink.picture = [](void* c, const char* srcName) -> uint16_t {
    auto* x = static_cast<Ctx*>(c);
    if (!x->pics || !srcName || x->image == kNotSet) {
      return 0;
    }
    int index = -1;
    if (strncmp(srcName, "mobi:", 5) == 0) {
      index = atoi(srcName + 5);
    } else {
      const char* emb = strstr(srcName, "kindle:embed:");
      if (emb) {
        index = base32(emb + 13);
      }
    }
    if (index < 1) {
      return 0;
    }
    const uint32_t ri = x->image + static_cast<uint32_t>(index - 1);
    if (ri >= x->recs->n) {
      return 0;
    }
    uint8_t* img = x->recs->read(*x->book, static_cast<uint16_t>(ri));
    if (!img || x->recs->len[ri] < 8) {
      free(img);
      return 0;
    }
    HalFile wf;
    if (!Storage.openFileForWrite("MOBI", "/.yomu/work.img", wf) ||
        wf.write(img, x->recs->len[ri]) != x->recs->len[ri]) {
      wf.close();
      free(img);
      return 0;
    }
    wf.close();
    free(img);
    HalFile rf;
    if (!Storage.openFileForRead("MOBI", "/.yomu/work.img", rf)) {
      return 0;
    }
    uint8_t mag[4]{};
    if (rf.read(mag, 4) != 4 || !rf.seekSet(0)) {
      return 0;
    }
    const bool jpeg = mag[0] == 0xFF && mag[1] == 0xD8;
    const bool png = mag[0] == 0x89 && mag[1] == 'P';
    const bool gif = mag[0] == 'G' && mag[1] == 'I';
    if (jpeg) {
      return x->pics->addJpeg(rf);
    }
    if (png || gif) {
      return x->pics->addPng(rf);
    }
    return 0;
  };

  HalFile raw;
  if (!Storage.openFileForRead("MOBI", rawPath, raw)) {
    error = "mobi";
    writer.close();
    return false;
  }
  const uint16_t begin = spineBegin > nSpine ? nSpine : spineBegin;
  const uint16_t end = (maxSpines == 0xFFFF || static_cast<uint32_t>(begin) + maxSpines > nSpine)
                           ? nSpine
                           : static_cast<uint16_t>(begin + maxSpines);
  sliceEnd = begin;
  uint16_t htmlOk = 0;
  for (uint16_t i = begin; i < end; ++i) {
    if (!writer.ok() || !ctx.ok) {
      break;
    }
    report(i, nSpine);
    const Part& part = plan.parts[i];
    uint32_t total = part.skelLen;
    for (uint16_t k = 0; k < part.nfrag; ++k) {
      total += plan.frags[part.frag0 + k].len;
    }
    if (total == 0 || total > kMaxPart) {
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    uint8_t* buf = static_cast<uint8_t*>(malloc(total));
    if (!buf) {
      error = "mobi";
      break;
    }
    uint32_t cur = 0;
    if (raw.seekSet(part.skelOff)) {
      const int got = raw.read(buf, part.skelLen);
      cur = got > 0 ? static_cast<uint32_t>(got) : 0;
    }
    uint32_t base = part.skelOff + part.skelLen;
    for (uint16_t k = 0; k < part.nfrag; ++k) {
      const Frag& fr = plan.frags[part.frag0 + k];
      uint32_t ins = fr.insert >= part.skelOff ? fr.insert - part.skelOff : cur;
      if (ins > cur) {
        ins = cur;
      }
      if (fr.len > total - cur) {
        break;
      }
      memmove(buf + ins + fr.len, buf + ins, cur - ins);
      if (!raw.seekSet(base) || raw.read(buf + ins, fr.len) != static_cast<int>(fr.len)) {
        memset(buf + ins, ' ', fr.len);
      }
      cur += fr.len;
      base += fr.len;
    }
    rewriteRecindex(buf, cur);
    // The html element carries hltr or vrtl. Sniff that, not the prepended sheet:
    // a later stylesheet names both writing modes, and the last one would win.
    WritingMode chapterMode = mode;
    if (cur > 0 && sniffHorizontal(reinterpret_cast<const char*>(buf), cur)) {
      chapterMode = WritingMode::HorizontalTb;
    } else if (cur > 0 && sniffVertical(reinterpret_cast<const char*>(buf), cur)) {
      chapterMode = WritingMode::VerticalRl;
      mode = WritingMode::VerticalRl;
    }
    HalFile xhtml;
    if (!Storage.openFileForWrite("MOBI", "/.yomu/work.xhtml", xhtml)) {
      free(buf);
      error = "mobi";
      break;
    }
    if (xhtml.write(buf, cur) != cur) {
      free(buf);
      xhtml.close();
      error = "disk full";
      break;
    }
    xhtml.close();
    free(buf);
    HalFile hf;
    if (!Storage.openFileForRead("MOBI", "/.yomu/work.xhtml", hf)) {
      error = "mobi";
      break;
    }
    const uint32_t at = writer.position();
    writeChapterMode(writer, chapterMode);
    for (uint16_t t = 0; t < nToc; ++t) {
      if (tocs[t].spine == i) {
        tocs[t].atomOff = at;
      }
    }
    HtmlIRResult ir{};
    auto readHf = [](void* c, char* dst, int max) -> int {
      return static_cast<HalFile*>(c)->read(dst, static_cast<size_t>(max));
    };
    htmlToAtomsPull(readHf, &hf, sink, &ir);
    if (!ctx.ok || !writer.ok()) {
      break;
    }
    if (ir.title[0]) {
      for (uint16_t t = 0; t < nToc; ++t) {
        if (tocs[t].spine == i && tocs[t].title[0] == 0) {
          snprintf(tocs[t].title, sizeof tocs[t].title, "%s", ir.title);
        }
      }
    }
    ++htmlOk;
    sliceEnd = static_cast<uint16_t>(i + 1);
    Atom gap{};
    gap.kind = AtomKind::PageBreak;
    writer.write(gap);
    yield();
  }
  if (!writer.ok() || !ctx.ok) {
    error = writer.diskFull() ? "disk full" : "atom write";
    writer.close();
    if (appendAtoms && atomKeep >= 4) {
      HalFile torn = Storage.open(atomPath, O_RDWR);
      if (torn) {
        torn.truncate(atomKeep);
      }
    } else if (atomPath[0]) {
      Storage.remove(atomPath);
    }
    return false;
  }
  report(sliceEnd, nSpine);
  writer.close();
  pics.end();
  const bool whole = spineBegin == 0 && maxSpines == 0xFFFF && !appendAtoms;
  if ((whole && htmlOk == 0) || (!whole && sliceEnd == begin && begin < nSpine)) {
    error = "no html";
    return false;
  }
  error = "";
  LOG_INF("MOBI", "'%s' spine=%u/%u", bookTitle, sliceEnd, nSpine);
  return true;
}

void MobiBook::close() {
  free(tocs);
  tocs = nullptr;
  nToc = 0;
  nSpine = 0;
  sliceEnd = 0;
}

}  // namespace ts
