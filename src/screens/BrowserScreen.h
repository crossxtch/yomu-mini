#pragma once

#include <BookFace.h>

#include <string>
#include <vector>

#include "core/Screen.h"

class BrowserScreen final : public Screen {
 public:
  enum class Mode : uint8_t { Books, Firmware };

 private:
  std::string path = "/";
  std::vector<std::string> entries;
  int index = 0;
  int window = 0;
  uint8_t shownMinute = 255;
  Mode mode = Mode::Books;
  BookFace cjk;

  void load();
  void activate();
  void goUp();
  void loadCjk();

 public:
  BrowserScreen(Gfx& gfx, MappedInput& input, const char* initialPath = "/", Mode mode = Mode::Books);
  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render() override;
  bool blocksSleep() const override { return mode == Mode::Firmware; }
};
