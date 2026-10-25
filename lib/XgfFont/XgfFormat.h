#pragma once

#include <cstdint>

// On-disk 2-bit em-box glyph font (XGF2). Little-endian.
// cmap is a sorted interval table; bitmaps are fixed-stride slots.

namespace xgf {

constexpr uint32_t kMagic = 0x32464758;  // "XGF2"
constexpr uint16_t kVersion = 1;
constexpr uint16_t kHeaderSize = 64;

constexpr uint16_t kFlag2bpp = 1u << 0;
constexpr uint16_t kFlagFixedSlot = 1u << 1;
constexpr uint16_t kFlagFrequencyIds = 1u << 2;

constexpr uint32_t kInkThreshold = 1;  // any non-white
constexpr uint16_t kMaxIntervals = 32767;
constexpr uint16_t kMaxGlyphs = 32767;

#pragma pack(push, 1)
struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t flags;
  uint8_t emPx;
  uint8_t rubyEmPx;
  uint8_t bpp;
  uint8_t reserved0;
  uint16_t bodyStride;
  uint16_t rubyStride;
  uint16_t bodyCount;
  uint16_t rubyCount;
  uint16_t intervalCount;
  uint16_t reserved1;
  uint32_t intervalsOff;
  uint32_t rubyMapOff;
  uint32_t bodyBitsOff;
  uint32_t rubyBitsOff;
  uint8_t grayLut[4];
  uint8_t reserved[20];
};

struct Interval {
  uint16_t first;
  uint16_t last;
  uint16_t glyphId;  // id of `first`; first+k → glyphId+k
};
#pragma pack(pop)

static_assert(sizeof(Header) == kHeaderSize, "XGF2 header must be 64 bytes");
static_assert(sizeof(Interval) == 6, "XGF2 interval must be 6 bytes");

inline uint16_t rowBytes(const uint8_t emPx) {
  return static_cast<uint16_t>((static_cast<unsigned>(emPx) + 3u) / 4u);
}

inline uint16_t slotStride(const uint8_t emPx) {
  return static_cast<uint16_t>(rowBytes(emPx) * emPx);
}

}  // namespace xgf
