#include "screens/FontsScreen.h"

#include <EpdFontFamily.h>
#include <Gfx.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/ReadingFont.h"
#include "core/Settings.h"
#include "core/UiList.h"
#include "core/UiText.h"
#include "core/fontIds.h"

FontsScreen::FontsScreen(Gfx& gfx, MappedInput& input) : Screen("Fonts", gfx, input) {}

namespace {

const char* faceLabel(const char* id) {
  if (id && strcmp(id, ReadingFont::kMinchoId) == 0) {
    return uiText::fontMincho;
  }
  if (id && strcmp(id, ReadingFont::kGothicId) == 0) {
    return uiText::fontGothic;
  }
  return id ? id : "";
}

bool faceActive(const char* id) {
  if (!id) {
    return false;
  }
  if (strcmp(id, ReadingFont::kMinchoId) == 0) {
    return settings.fontFile[0] == '\0' || strcmp(settings.fontFile, ReadingFont::kMinchoId) == 0;
  }
  return strcmp(settings.fontFile, id) == 0;
}

}  // namespace

void FontsScreen::load() {
  names.clear();
  names.reserve(34);
  names.emplace_back(ReadingFont::kMinchoId);
  names.emplace_back(ReadingFont::kGothicId);
  std::vector<std::string> files;
  files.reserve(32);
  HalFile dir = Storage.open(ReadingFont::kDir);
  if (dir && dir.isDirectory()) {
    char name[HalFile::kMaxNameBytes];
    for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory() || file.getName(name, sizeof(name)) == 0) {
        continue;
      }
      if (!ReadingFont::isFontFilename(name)) {
        continue;
      }
      files.emplace_back(name);
      if (files.size() >= 32) {
        break;
      }
    }
  }
  std::sort(files.begin(), files.end());
  for (auto& file : files) {
    names.push_back(std::move(file));
  }
  index = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    if (faceActive(names[i].c_str())) {
      index = static_cast<int>(i);
      break;
    }
  }
  window = 0;
  LOG_INF("FONT", "%u faces, active='%s'", static_cast<unsigned>(names.size()), settings.fontFile);
}

void FontsScreen::loadCjk() {
  if (cjk.loaded()) {
    return;
  }
  if (!ReadingFont::loadUi(cjk)) {
    LOG_INF("FONT", "No UI font");
  }
}

void FontsScreen::onEnter() {
  Screen::onEnter();
  ReadingFont::migrate();
  loadCjk();
  load();
  requestUpdate();
}

void FontsScreen::onExit() {
  cjk.close();
  Screen::onExit();
}

void FontsScreen::activate() {
  if (names.empty()) {
    return;
  }
  if (!ReadingFont::setActive(names[static_cast<size_t>(index)].c_str())) {
    return;
  }
  finish();
}

void FontsScreen::loop() {
  if (input.wasReleased(MappedInput::Button::Back)) {
    finish();
    return;
  }
  const int count = static_cast<int>(names.size());
  if (ui::applyDelta(index, input.consumeNavigationDelta(), count)) {
    requestUpdate();
  } else if (count > 0 && input.wasReleased(MappedInput::Button::Confirm)) {
    activate();
  }
}

void FontsScreen::render() {
  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI_BOLD, 8, uiText::readingFont);

  const int rowH = gfx.lineHeight(FONT_UI) + 8;
  const int top = 40;
  const int rows = (gfx.height() - top - 24) / rowH;
  if (names.empty()) {
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2, uiText::noFonts);
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 28, uiText::uploadFontHint);
    presentUi();
    return;
  }

  ui::followWindow(window, index, rows);
  const int last = std::min(window + rows, static_cast<int>(names.size()));
  char row[96];
  auto fillRow = [&](const int i) {
    const char* name = names[static_cast<size_t>(i)].c_str();
    snprintf(row, sizeof(row), "%s%s", faceActive(name) ? "* " : "", faceLabel(name));
  };
  if (cjk.loaded()) {
    const EpdFontFamily* ui = gfx.font(FONT_UI);
    uint16_t ids[192];
    uint16_t n = 0;
    for (int i = window; i < last; ++i) {
      fillRow(i);
      const unsigned char* p = reinterpret_cast<const unsigned char*>(row);
      uint32_t cp = 0;
      while ((cp = utf8NextCodepoint(&p)) && n < 192) {
        if (ui && ui->hasCodepoint(cp)) {
          continue;
        }
        const uint16_t id = cjk.glyphId(cp);
        if (id != 0xFFFF) {
          ids[n++] = id;
        }
      }
    }
    cjk.prewarm(ids, n, false);
  }
  for (int i = window; i < last; ++i) {
    fillRow(i);
    ui::drawRow(gfx, top + (i - window) * rowH, rowH, row, i == index, cjk.loaded() ? &cjk : nullptr);
  }
  presentUi();
}
