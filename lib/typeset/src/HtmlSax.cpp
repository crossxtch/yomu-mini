#include "HtmlSax.h"

#include <cstring>

namespace ts {

char HtmlSax::lower(const char c) {
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c - 'A' + 'a');
  }
  return c;
}

void HtmlSax::bind(const char* d, const size_t n) {
  data = d;
  len = n;
  i = 0;
  pull = nullptr;
  pullCtx = nullptr;
  winLen = 0;
  winI = 0;
  kind = Kind::Eof;
  name[0] = 0;
  ntext = 0;
  nattrs = 0;
  pos = 0;
}

void HtmlSax::bindPull(ReadFn read, void* ctx) {
  data = nullptr;
  len = 0;
  i = 0;
  pull = read;
  pullCtx = ctx;
  winLen = 0;
  winI = 0;
  kind = Kind::Eof;
  name[0] = 0;
  ntext = 0;
  nattrs = 0;
  pos = 0;
}

bool HtmlSax::ensure(const size_t n) {
  if (data) {
    return i + n <= len || i < len;
  }
  if (!pull) {
    return false;
  }
  while (winLen - winI < n) {
    if (winI > 0 && winI < winLen) {
      memmove(win, win + winI, winLen - winI);
      winLen -= winI;
      winI = 0;
    } else if (winI >= winLen) {
      winI = 0;
      winLen = 0;
    }
    if (winLen >= sizeof(win)) {
      break;
    }
    const int g = pull(pullCtx, win + winLen, static_cast<int>(sizeof(win) - winLen));
    if (g <= 0) {
      break;
    }
    winLen += static_cast<size_t>(g);
  }
  return winI < winLen;
}

int HtmlSax::peek() {
  if (!ensure(1)) {
    return -1;
  }
  if (data) {
    if (i >= len) {
      return -1;
    }
    return static_cast<unsigned char>(data[i]);
  }
  return static_cast<unsigned char>(win[winI]);
}

int HtmlSax::take() {
  const int c = peek();
  if (c < 0) {
    return -1;
  }
  if (data) {
    ++i;
  } else {
    ++winI;
  }
  ++pos;
  return c;
}

void HtmlSax::skipWs() {
  for (;;) {
    const int c = peek();
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      break;
    }
    take();
  }
}

bool HtmlSax::parseComment() {
  // caller already consumed '!' of "!--" or we consume from current
  while (ensure(3)) {
    if (peek() == '-' ) {
      take();
      if (peek() == '-') {
        take();
        if (peek() == '>') {
          take();
          return true;
        }
      }
      continue;
    }
    take();
  }
  return true;
}

bool HtmlSax::parseTag() {
  const uint32_t start = pos;
  if (take() != '<') {
    return false;
  }
  int c = peek();
  if (c == '!') {
    take();
    if (peek() == '-' ) {
      take();
      if (peek() == '-') {
        take();
        parseComment();
        return next();
      }
    }
    while ((c = peek()) >= 0 && c != '>') {
      take();
    }
    if (peek() == '>') {
      take();
    }
    return next();
  }
  if (c == '?') {
    take();
    while ((c = peek()) >= 0 && c != '>') {
      take();
    }
    if (peek() == '>') {
      take();
    }
    return next();
  }
  bool end = false;
  if (c == '/') {
    end = true;
    take();
  }
  skipWs();
  size_t nameN = 0;
  char raw[24];
  while ((c = peek()) >= 0 && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '>' && c != '/') {
    if (nameN + 1 < sizeof(raw)) {
      raw[nameN++] = static_cast<char>(take());
    } else {
      take();
    }
  }
  raw[nameN] = 0;
  const char* ns = raw;
  for (size_t k = 0; k < nameN; ++k) {
    if (raw[k] == ':') {
      ns = raw + k + 1;
      nameN -= k + 1;
      break;
    }
  }
  size_t nn = nameN < sizeof(name) - 1 ? nameN : sizeof(name) - 1;
  for (size_t k = 0; k < nn; ++k) {
    name[k] = lower(ns[k]);
  }
  name[nn] = 0;
  nattrs = 0;
  bool empty = false;
  while ((c = peek()) >= 0 && c != '>') {
    if (c == '/' && ensure(2)) {
      take();
      if (peek() == '>') {
        take();
        empty = true;
        kind = Kind::Empty;
        pos = start;
        return true;
      }
      continue;
    }
    skipWs();
    c = peek();
    if (c < 0 || c == '>') {
      break;
    }
    char key[24];
    size_t keyN = 0;
    while ((c = peek()) >= 0 && c != '=' && c != ' ' && c != '>' && c != '/') {
      if (keyN + 1 < sizeof(key)) {
        key[keyN++] = lower(static_cast<char>(take()));
      } else {
        take();
      }
    }
    key[keyN] = 0;
    const char* ks = key;
    size_t kn = keyN;
    for (size_t k = 0; k < keyN; ++k) {
      if (key[k] == ':') {
        ks = key + k + 1;
        kn = keyN - k - 1;
        break;
      }
    }
    skipWs();
    char val[256];
    size_t valN = 0;
    if (peek() == '=') {
      take();
      skipWs();
      const int q = peek();
      if (q == '"' || q == '\'') {
        take();
        while ((c = peek()) >= 0 && c != q) {
          if (valN + 1 < sizeof(val)) {
            val[valN++] = static_cast<char>(take());
          } else {
            take();
          }
        }
        if (peek() == q) {
          take();
        }
      } else {
        while ((c = peek()) >= 0 && c != ' ' && c != '>') {
          if (valN + 1 < sizeof(val)) {
            val[valN++] = static_cast<char>(take());
          } else {
            take();
          }
        }
      }
    }
    val[valN] = 0;
    if (nattrs + kn + valN + 3 < sizeof(attrs)) {
      attrs[nattrs++] = static_cast<char>(kn);
      memcpy(attrs + nattrs, ks, kn);
      nattrs = static_cast<uint16_t>(nattrs + kn);
      attrs[nattrs++] = static_cast<char>(valN > 255 ? 255 : valN);
      const size_t copyN = valN > 255 ? 255 : valN;
      memcpy(attrs + nattrs, val, copyN);
      nattrs = static_cast<uint16_t>(nattrs + copyN);
    }
  }
  if (peek() == '>') {
    take();
  }
  kind = end ? Kind::End : (empty ? Kind::Empty : Kind::Start);
  pos = start;
  return true;
}

bool HtmlSax::parseText() {
  const uint32_t start = pos;
  ntext = 0;
  auto utf8Need = [](int c) -> int {
    if (c < 0) {
      return 0;
    }
    if ((c & 0x80) == 0) {
      return 1;
    }
    if ((c & 0xE0) == 0xC0) {
      return 2;
    }
    if ((c & 0xF0) == 0xE0) {
      return 3;
    }
    if ((c & 0xF8) == 0xF0) {
      return 4;
    }
    return 1;
  };
  while (peek() >= 0 && peek() != '<') {
    int c = peek();
    if (c == '&') {
      if (ntext + 4 >= sizeof(text)) {
        break;
      }
      take();
      char ent[8];
      size_t en = 0;
      while (peek() >= 0 && peek() != ';' && peek() != '<' && en + 1 < sizeof(ent)) {
        ent[en++] = static_cast<char>(take());
      }
      ent[en] = 0;
      if (peek() == ';') {
        take();
      }
      if (strcmp(ent, "amp") == 0) {
        text[ntext++] = '&';
      } else if (strcmp(ent, "lt") == 0) {
        text[ntext++] = '<';
      } else if (strcmp(ent, "gt") == 0) {
        text[ntext++] = '>';
      } else if (strcmp(ent, "quot") == 0) {
        text[ntext++] = '"';
      } else if (strcmp(ent, "apos") == 0) {
        text[ntext++] = '\'';
      } else if (strcmp(ent, "nbsp") == 0) {
        text[ntext++] = static_cast<char>(0xC2);
        text[ntext++] = static_cast<char>(0xA0);
      } else if (ent[0] == '#') {
        uint32_t cp = 0;
        size_t ei = 1;
        const bool hex = ent[1] == 'x' || ent[1] == 'X';
        if (hex) {
          ei = 2;
        }
        while (ent[ei]) {
          const char d = ent[ei++];
          if (hex) {
            const int v = (d >= '0' && d <= '9')   ? d - '0'
                          : (d >= 'a' && d <= 'f') ? d - 'a' + 10
                          : (d >= 'A' && d <= 'F') ? d - 'A' + 10
                                                   : -1;
            if (v < 0) {
              break;
            }
            cp = (cp << 4) | static_cast<uint32_t>(v);
          } else if (d >= '0' && d <= '9') {
            cp = cp * 10 + static_cast<uint32_t>(d - '0');
          } else {
            break;
          }
        }
        if (cp < 0x80) {
          text[ntext++] = static_cast<char>(cp);
        } else if (cp < 0x800) {
          text[ntext++] = static_cast<char>(0xC0 | (cp >> 6));
          text[ntext++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          text[ntext++] = static_cast<char>(0xE0 | (cp >> 12));
          text[ntext++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          text[ntext++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
      } else {
        text[ntext++] = '&';
      }
      continue;
    }
    const int need = utf8Need(c);
    if (need < 1 || ntext + static_cast<uint16_t>(need) >= sizeof(text)) {
      break;
    }
    for (int k = 0; k < need; ++k) {
      const int b = take();
      if (b < 0) {
        break;
      }
      text[ntext++] = static_cast<char>(b);
    }
  }
  text[ntext] = 0;
  kind = Kind::Text;
  pos = start;
  return ntext > 0;
}

bool HtmlSax::next() {
  for (;;) {
    const int c = peek();
    if (c < 0) {
      kind = Kind::Eof;
      return false;
    }
    if (c == '<') {
      return parseTag();
    }
    if (parseText()) {
      return true;
    }
  }
}

bool HtmlSax::attr(const char* key, char* out, const size_t cap) const {
  if (!out || cap == 0) {
    return false;
  }
  out[0] = 0;
  size_t p = 0;
  const size_t keyN = strlen(key);
  while (p < nattrs) {
    const uint8_t kn = static_cast<uint8_t>(attrs[p++]);
    if (p + kn > nattrs) {
      break;
    }
    const bool match = kn == keyN && memcmp(attrs + p, key, kn) == 0;
    p += kn;
    if (p >= nattrs) {
      break;
    }
    const uint8_t vn = static_cast<uint8_t>(attrs[p++]);
    if (p + vn > nattrs) {
      break;
    }
    if (match) {
      size_t n = vn;
      if (n >= cap) {
        n = cap - 1;
      }
      memcpy(out, attrs + p, n);
      out[n] = 0;
      return true;
    }
    p += vn;
  }
  return false;
}

bool HtmlSax::hasClass(const char* want) const {
  char cls[256];
  if (!attr("class", cls, sizeof(cls))) {
    return false;
  }
  const size_t wn = strlen(want);
  const char* s = cls;
  while (*s) {
    while (*s == ' ') {
      ++s;
    }
    const char* e = s;
    while (*e && *e != ' ') {
      ++e;
    }
    if (static_cast<size_t>(e - s) == wn && memcmp(s, want, wn) == 0) {
      return true;
    }
    s = e;
  }
  return false;
}

bool HtmlSax::hasClassPrefix(const char* prefix) const {
  char cls[256];
  if (!attr("class", cls, sizeof(cls))) {
    return false;
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
    if (static_cast<size_t>(e - s) >= pn && memcmp(s, prefix, pn) == 0) {
      return true;
    }
    s = e;
  }
  return false;
}

bool HtmlSax::attrContains(const char* key, const char* needle) const {
  char v[256];
  if (!attr(key, v, sizeof(v))) {
    return false;
  }
  return strstr(v, needle) != nullptr;
}

}  // namespace ts
