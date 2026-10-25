#include "BakedFont.h"

extern "C" {
extern const uint8_t baked_mincho_start[];
extern const uint8_t baked_mincho_end[];
extern const uint8_t baked_gothic_start[];
extern const uint8_t baked_gothic_end[];
}

namespace BakedFont {

bool bytes(const Kind kind, const uint8_t*& data, size_t& size) {
  const uint8_t* begin = nullptr;
  const uint8_t* end = nullptr;
  switch (kind) {
    case Kind::Mincho:
      begin = baked_mincho_start;
      end = baked_mincho_end;
      break;
    case Kind::Gothic:
      begin = baked_gothic_start;
      end = baked_gothic_end;
      break;
  }
  if (!begin || !end || end <= begin) {
    data = nullptr;
    size = 0;
    return false;
  }
  data = begin;
  size = static_cast<size_t>(end - begin);
  return true;
}

}  // namespace BakedFont
