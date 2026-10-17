#pragma once

#include <ReadFace.h>

#include "core/Screen.h"

class HomeScreen final : public Screen {
  int index = 0;
  int itemCount = 0;
  uint8_t shownMinute = 255;
  ReadFace cjk;

  void refreshMenu();
  void loadCjk();

 public:
  HomeScreen(Gfx& gfx, MappedInput& input) : Screen("Home", gfx, input) {}
  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render() override;
};
