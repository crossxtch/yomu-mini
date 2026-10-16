#pragma once

#include <HalStorage.h>

#include <cstdint>

namespace ts {

// PIC2. 16-byte header, then a size table, then id-1 full portrait pages.
// Pixel 0 is white and 3 is black, four pixels per byte, MSB first.
// Each size entry is uint16 width, uint16 height: the pixels contain-fit used.
// A PIC1 sidecar fails the magic check and is rebuilt.
constexpr uint32_t kPictureMagic = 0x32434950u;
constexpr uint16_t kMaxPictures = 48;
constexpr uint32_t kPictureHeader = 16;
constexpr uint32_t kPictureSizeBytes = kMaxPictures * 4u;
constexpr uint32_t kPicturePixels = kPictureHeader + kPictureSizeBytes;

class PictureWriter {
 public:
  PictureWriter() = default;
  ~PictureWriter() { end(); }
  PictureWriter(const PictureWriter&) = delete;
  PictureWriter& operator=(const PictureWriter&) = delete;

  // `append` keeps ids already stored in a PIC2 file of the same portrait size.
  bool begin(const char* path, uint16_t pageW, uint16_t pageH, bool append = false);
  void end();
  bool ready() const { return out.isOpen() && pageW > 0; }
  uint16_t count() const { return n; }

  // Stream-decode one baseline JPEG or non-interlaced PNG. Returns a 1-based id, or 0.
  uint16_t addJpeg(HalFile& image);
  uint16_t addPng(HalFile& image);

 private:
  HalFile out;
  uint16_t pageW = 0;
  uint16_t pageH = 0;
  uint16_t n = 0;
  uint16_t rowBytes = 0;
  uint32_t pageBytes = 0;

  bool seekSlot();
  bool writeZeroRows(uint8_t* zeros, uint16_t nrows);
  uint16_t commit(uint16_t srcW, uint16_t srcH);
};

// Copy one stored page into `dst` (pageW * pageH / 4 bytes). Id is 1-based.
bool pictureRead(const char* path, uint16_t id, uint16_t pageW, uint16_t pageH, uint8_t* dst, size_t dstBytes);

// Source pixels stored for image `id`. False when the record is missing.
bool pictureSize(const char* path, uint16_t id, int* w, int* h);

// JPEG SOF or PNG IHDR. Seeks back to the start. False when the size is unknown.
bool imagePixelSize(HalFile& image, uint16_t& w, uint16_t& h);

}  // namespace ts
