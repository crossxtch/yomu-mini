#pragma once

#include <BookFace.h>

#include <string>
#include <vector>

#include "core/Screen.h"

class FontsScreen final : public Screen {
  std::vector<std::string> names;
  int index = 0;
  int window = 0;
  BookFace cjk;

  void load();
  void loadCjk();
  void activate();

 public:
  FontsScreen(Gfx& gfx, MappedInput& input);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render() override;
};
