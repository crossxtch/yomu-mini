#pragma once

#include <HalStorage.h>
#include <ReadFace.h>

#include <cstdint>
#include <memory>

#include "XgfFormat.h"

class Gfx;

// Prebaked reading face. The XGF2 image, or a YGZF zlib wrap of one, is held in
// one PSRAM block. Slots are fixed 2-bit em boxes. Freed in close().
class XgfFont {
 public:
  using Plane = ReadFace::Plane;
  using SlotProbe = ReadFace::SlotProbe;
  using CacheStats = ReadFace::CacheStats;

  static constexpr uint32_t kMaxImage = 4u * 1024u * 1024u;

  XgfFont() = default;
  ~XgfFont() { close(); }

  XgfFont(const XgfFont&) = delete;
  XgfFont& operator=(const XgfFont&) = delete;

  bool load(const char* path, bool wantRuby = true);
  // YGZF: uint32 magic, uint32 raw size, then a zlib wrapper around an XGF2.
  // Inflated with puff (raw DEFLATE). The zlib header and Adler checksum stay
  // outside the inflate.
  bool loadPacked(const uint8_t* packed, size_t packedBytes, bool wantRuby = true);
  void close();
  bool loaded() const { return opened; }
  // The image stays in PSRAM. EPUB ingest does not need these bytes back.
  void releaseMaps() {}
  bool restoreMaps() { return opened; }
  void releaseFile() {}
  bool reopenFile() { return opened; }

  uint8_t emPx() const { return header.emPx; }
  uint8_t rubyEmPx() const { return header.rubyEmPx; }
  const char* lastError() const { return error; }

  uint16_t glyphId(uint32_t cp) const;
  bool hasGlyph(const uint32_t cp) const { return glyphId(cp) != 0xFFFF; }
  uint16_t rubySlot(uint16_t bodyId) const;

  void prewarm(const uint16_t* bodyIds, uint16_t count, bool ruby);
  void setSdReads(bool allow) { (void)allow; }

  bool probe(uint16_t bodyId, bool ruby, SlotProbe& out) const;
  CacheStats cacheStats() const;

  // Fixed em. `px` is returned when the codepoint is in the face.
  int16_t advancePx(uint32_t cp, int px);

  bool blit(Gfx& gfx, int x, int y, uint16_t bodyId, bool ruby, bool rotate90, Plane plane);
  bool blitCell(Gfx& gfx, int x, int y, int cellW, int cellH, uint16_t bodyId, int px, bool vertical, bool bold,
                bool rotate90, bool yakumono, Plane plane);
  bool blitBox(Gfx& gfx, int x, int y, uint16_t bodyId, int size, Plane plane);
  int drawUtf8(Gfx& gfx, int fontId, int x, int y, const char* text, bool black, int maxX = 0);

 private:
  struct Hi {
    uint16_t begin = 0;
    uint16_t count = 0;
  };

  // Owned PSRAM image of the uncompressed XGF2. Freed in close().
  uint8_t* image = nullptr;
  uint32_t imageSize = 0;
  xgf::Header header{};
  std::unique_ptr<xgf::Interval[]> intervals;
  std::unique_ptr<uint16_t[]> rubyMap;
  Hi hi[256]{};
  bool opened = false;
  bool wantRuby = true;
  const char* error = "not loaded";
  mutable uint32_t statHit = 0;

  bool adopt(uint8_t* bytes, uint32_t n);
  bool parse();
  void buildHi();
  const uint8_t* slotBytes(uint16_t bodyId, bool ruby) const;
  bool blitUi(Gfx& gfx, int x, int y, uint16_t bodyId, int size, bool black);
};
