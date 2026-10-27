#pragma once

#include "AtomFile.h"
#include "HtmlIR.h"

#include <cstdint>

namespace ts {

// DRM-free Mobipocket and KF8 (the bytes behind .mobi and .azw3).
// Encrypted and fixed-layout files fail open(). The markup is handed to the
// same HTML atom writer an EPUB spine item uses.
class MobiBook {
 public:
  static constexpr uint16_t kMaxSpine = 2048;

  MobiBook() = default;
  ~MobiBook() { close(); }
  MobiBook(const MobiBook&) = delete;
  MobiBook& operator=(const MobiBook&) = delete;

  using ProgressFn = void (*)(void* ctx, uint16_t done, uint16_t total);

  // MRP6. Each stylesheet flow is scanned on its own, including one larger
  // than 64KB. A rule stores an em width, the sides of a partial border, a
  // family, and a writing-mode guard. An older plan rebuilds.
  static constexpr char kPlanMagic[4] = {'M', 'R', 'P', '6'};

  // Same slice contract as EpubBook::open. A later slice reuses the unpacked
  // markup from the first slice.
  bool open(const char* path, const char* atomPath, ProgressFn progress = nullptr, void* progressCtx = nullptr,
            uint16_t pageW = 0, uint16_t pageH = 0, const char* picturePath = nullptr, uint16_t spineBegin = 0,
            uint16_t maxSpines = 0xFFFF, bool appendAtoms = false, uint32_t atomKeep = 0);
  void close();

  const char* title() const { return bookTitle; }
  WritingMode bookMode() const { return mode; }
  void preferVertical() { verticalPreferred = true; }
  uint16_t spineCount() const { return nSpine; }
  uint16_t ingestedEnd() const { return sliceEnd; }
  const char* lastError() const { return error; }

  struct TocEntry {
    char title[80]{};
    char href[96]{};
    uint32_t atomOff = 0xFFFFFFFFu;
    uint16_t spine = 0xFFFFu;
    char fragment[40]{};
    uint8_t depth = 0;
  };
  uint16_t tocCount() const { return nToc; }
  const TocEntry& toc(uint16_t i) const { return tocs[i]; }

 private:
  TocEntry* tocs = nullptr;
  uint16_t nToc = 0;
  uint16_t nSpine = 0;
  uint16_t sliceEnd = 0;
  char bookTitle[128]{};
  WritingMode mode = WritingMode::VerticalRl;
  const char* error = "closed";
  bool verticalPreferred = false;
};

}  // namespace ts
