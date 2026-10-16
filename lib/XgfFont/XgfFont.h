#pragma once

#include <FtFont.h>
#include <HalStorage.h>

#include <cstdint>

class Gfx;

// Reading face. A TrueType file on the SD card is rasterized on device into
// fixed 2-bit em boxes and cached in PSRAM. The typesetter still blits those
// boxes one em at a time.
class XgfFont {
 public:
  enum class Plane : uint8_t { Ink, Lsb, Msb };

  // A 40 px 2-bit cell is 400 bytes. 1 MB holds about 2500 body glyphs.
  static constexpr uint32_t kLruBytes = 1024 * 1024;
  static constexpr uint32_t kRubyLruBytes = 256 * 1024;
  static constexpr uint32_t kUiLruBytes = 128 * 1024;

  // 13 pt at the X4 Classic's 219 PPI. Ruby is half an em, rounded up.
  static constexpr uint8_t kEmPx = 40;
  static constexpr uint8_t kRubyEmPx = 20;

  XgfFont() = default;
  ~XgfFont() { close(); }

  XgfFont(const XgfFont&) = delete;
  XgfFont& operator=(const XgfFont&) = delete;

  bool load(const char* path, uint32_t lruMaxBytes = kLruBytes, bool wantRuby = true);
  void close();
  bool loaded() const { return opened; }
  // Drop/restore the FreeType faces so EPUB ingest can use the heap. The file
  // bytes (or the stream handle) stay.
  void releaseMaps();
  bool restoreMaps();
  // Close the SD handle without dropping the face bytes or the path.
  void releaseFile();
  bool reopenFile();

  uint8_t emPx() const { return emPx_; }
  uint8_t rubyEmPx() const { return rubyEmPx_; }
  const char* lastError() const { return error; }

  // Unicode → FreeType glyph id. 0xFFFF = missing.
  uint16_t glyphId(uint32_t cp) const;
  bool hasGlyph(const uint32_t cp) const { return glyphId(cp) != 0xFFFF; }

  // Every body glyph can be drawn at ruby size. 0xFFFF when ruby is off.
  uint16_t rubySlot(uint16_t bodyId) const;

  // Rasterize misses. ids are body glyph ids.
  void prewarm(const uint16_t* bodyIds, uint16_t count, bool ruby);

  // When false, a streamed-face miss stays a miss and does not touch the SD.
  void setSdReads(const bool allow) { allowSdReads = allow; }

  struct SlotProbe {
    uint16_t ink = 0;
    uint16_t area = 0;
    uint32_t hash = 0;
    bool cached = false;
  };
  bool probe(uint16_t bodyId, bool ruby, SlotProbe& out) const;

  struct CacheStats {
    uint16_t bodyUsed = 0;
    uint16_t bodyCap = 0;
    uint16_t rubyUsed = 0;
    uint16_t rubyCap = 0;
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint32_t rejected = 0;
    uint32_t blocked = 0;
    uint32_t sdReads = 0;
    uint32_t sdBytes = 0;
    uint32_t sdUs = 0;
  };
  CacheStats cacheStats() const;

  // Blit one em (or ruby) slot at logical top-left. rotate90 is 90° CW, y-down.
  bool blit(Gfx& gfx, int x, int y, uint16_t bodyId, bool ruby, bool rotate90, Plane plane);

  // Scale a body glyph into a size×size box. Used for 縦中横.
  bool blitBox(Gfx& gfx, int x, int y, uint16_t bodyId, int size, Plane plane);

  // 1-bit UI: Latin from Gfx fontId, other glyphs from this face, scaled to the
  // UI ascender. y matches Gfx::drawText. Stops before maxX (0 = no clip).
  int drawUtf8(Gfx& gfx, int fontId, int x, int y, const char* text, bool black, int maxX = 0);

 private:
  struct LruEntry {
    uint16_t id = 0xFFFF;
    uint16_t recency = 0;
  };
  struct Lru {
    uint8_t* pixels = nullptr;
    LruEntry* entries = nullptr;
    uint16_t cap = 0;
    uint16_t used = 0;
    uint16_t clock = 1;
    uint16_t stride = 0;
  };

  static unsigned long streamRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);

  char filepath[256]{};
  const char* error = "not loaded";
  HalFile file;
  uint8_t* fileBytes = nullptr;
  uint32_t fileSize = 0;
  uint32_t prefixBytes = 0;
  bool streamed = false;
  bool opened = false;
  bool facesReady = false;
  bool wantRuby = true;

  freeink::font::FtFont bodyFace;
  freeink::font::FtFont rubyFace;
  uint8_t emPx_ = kEmPx;
  uint8_t rubyEmPx_ = kRubyEmPx;

  Lru bodyLru;
  Lru rubyLru;
  uint32_t lruLimit = kLruBytes;
  bool allowSdReads = true;
  uint32_t statHit = 0;
  uint32_t statMiss = 0;
  uint32_t statFf = 0;
  uint32_t statBlocked = 0;
  uint32_t statSdReads = 0;
  uint32_t statSdBytes = 0;
  uint32_t statSdUs = 0;

  bool loadBytes();
  void freeBytes();
  bool initFaces();
  void freeFaces();
  bool allocLru();
  void freeLru();
  bool allocTable(Lru& t, uint16_t cap, uint16_t stride);
  void freeTable(Lru& t);
  const uint8_t* cacheIn(Lru& t, uint16_t bodyId, bool ruby);
  const uint8_t* cacheSlot(uint16_t bodyId, bool ruby);
  bool rasterizeSlot(uint16_t bodyId, bool ruby, uint8_t* dest);
  bool blitUi(Gfx& gfx, int x, int y, uint16_t bodyId, int size, bool black);
};
