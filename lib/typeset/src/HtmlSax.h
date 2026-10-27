#pragma once

#include <cstddef>
#include <cstdint>

namespace ts {

// Pull tokenizer over memory or a refill callback (SD file).
class HtmlSax {
 public:
  enum class Kind : uint8_t { Start, End, Empty, Text, Eof };
  using ReadFn = int (*)(void* ctx, char* dst, int max);

  void bind(const char* data, size_t len);
  void bindPull(ReadFn read, void* ctx);
  bool next();

  Kind kind = Kind::Eof;
  char name[24]{};
  char text[192]{};
  uint16_t ntext = 0;
  uint32_t pos = 0;

  bool attr(const char* key, char* out, size_t cap) const;
  bool hasClass(const char* want) const;
  bool hasClassPrefix(const char* prefix) const;
  bool attrContains(const char* key, const char* needle) const;

 private:
  const char* data = nullptr;
  size_t len = 0;
  size_t i = 0;
  ReadFn pull = nullptr;
  void* pullCtx = nullptr;
  char win[1024]{};
  size_t winLen = 0;
  size_t winI = 0;
  char attrs[384]{};
  uint16_t nattrs = 0;

  bool ensure(size_t n);
  int peek();
  int take();
  void skipWs();
  bool parseTag();
  bool parseText();
  bool parseComment();
  static char lower(char c);
};

}  // namespace ts
