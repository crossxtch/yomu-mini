#pragma once

#include "FaceCss.h"
#include "Layout.h"

#include <cstddef>
#include <cstdint>

namespace ts {

struct AtomSink {
  void* ctx = nullptr;
  bool (*emit)(void* ctx, const Atom& atom, uint32_t pos) = nullptr;
  // Full-page picture. Return a 1-based id, or 0 to keep a blank page break.
  uint16_t (*picture)(void* ctx, const char* src) = nullptr;
  const FaceRule* rules = nullptr;
  uint16_t nRules = 0;
  // id or a name, at the atom-file offset of the next atom. Layout ignores it.
  void (*anchor)(void* ctx, const char* id) = nullptr;
  // Longer side in pixels. Negative means the file was not measured.
  // Null keeps the device image branch: alt text, a CID, or a full-page picture.
  int (*imageLongSide)(void* ctx, const char* src) = nullptr;
  // An h1–h6 id, before that heading's opening column break.
  void (*heading)(void* ctx, const char* id) = nullptr;
  // The whole document is at least 2000 characters. A `p-toc` class on that
  // chapter is not a contents list, so paragraphs stay in normal flow.
  bool longText = false;
  // A pre-paginated spine item. Its picture is a page of its own.
  bool forcePlate = false;
};

struct HtmlIRResult {
  WritingMode mode = WritingMode::VerticalRl;
  bool compactColumns = false;
  bool hasMode = false;
  char title[96]{};
};

// Convert 電書協/Calibre XHTML to the lazahata atom stream.
bool htmlToAtoms(const char* data, size_t len, AtomSink sink, HtmlIRResult* result = nullptr);
bool htmlToAtomsPull(int (*read)(void* ctx, char* dst, int max), void* ctx, AtomSink sink,
                     HtmlIRResult* result = nullptr);

// html/body class, else a `<style>` writing-mode. False when the item does not say.
// The EPUB opener then checks a linked stylesheet for the html or body class.
bool sniffWritingMode(const char* data, size_t len, WritingMode& mode);

void splitReading(const uint32_t* rt, uint8_t rtN, uint8_t baseN, uint32_t* out, uint8_t* outCount /* per base, cap 4 */);

}  // namespace ts
