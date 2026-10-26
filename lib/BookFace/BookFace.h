#pragma once

#include <ReadFace.h>

#include <cstddef>
#include <cstdint>
#include <memory>

class Gfx;

// The face a book or a list draws with. A TrueType file on the card is
// rasterized on device.
class BookFace {
 public:
  using Plane = ReadFace::Plane;
  using SlotProbe = ReadFace::SlotProbe;
  using CacheStats = ReadFace::CacheStats;

  BookFace() = default;
  ~BookFace() { close(); }

  BookFace(const BookFace&) = delete;
  BookFace& operator=(const BookFace&) = delete;

  bool load(const char* path, uint32_t lruMaxBytes = ReadFace::kLruBytes, bool wantRuby = true);
  void close();
  bool loaded() const { return static_cast<bool>(raster); }
  const char* lastError() const { return error; }

  void releaseMaps();
  bool restoreMaps();
  void releaseFile();
  bool reopenFile();

  uint8_t emPx() const;
  uint8_t rubyEmPx() const;
  uint16_t glyphId(uint32_t cp) const;
  void prewarm(const uint16_t* bodyIds, uint16_t count, bool ruby);
  void setSdReads(bool allow);
  bool probe(uint16_t bodyId, bool ruby, SlotProbe& out) const;
  CacheStats cacheStats() const;
  int16_t advancePx(uint32_t cp, int px);

  bool blit(Gfx& gfx, int x, int y, uint16_t bodyId, bool ruby, bool rotate90, Plane plane);
  bool blitCell(Gfx& gfx, int x, int y, int cellW, int cellH, uint16_t bodyId, int px, bool vertical, bool bold,
                bool rotate90, bool yakumono, Plane plane);
  bool blitBox(Gfx& gfx, int x, int y, uint16_t bodyId, int size, Plane plane);
  int drawUtf8(Gfx& gfx, int fontId, int x, int y, const char* text, bool black, int maxX = 0);

  // Eight hex chars. Page indexes are per face, because advances differ.
  void metricsId(char* out, size_t outSize) const;

 private:
  std::unique_ptr<ReadFace> raster;
  char key[192]{};
  const char* error = "not loaded";
};
