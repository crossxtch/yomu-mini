#pragma once

#include <cstdint>
#include <string>

namespace ts {

struct ChapterInfo {
  std::string name;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  // False until this chapter's atom offset is inside the indexed range.
  bool ready = false;
  // False when the next chapter in atom order is not on a page yet, and for
  // the last chapter until the whole book is indexed.
  bool endKnown = false;
};

}  // namespace ts
