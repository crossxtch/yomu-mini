#pragma once

#include <HalGPIO.h>

#include <cstdint>

class MappedInput {
 public:
  // PageBack/PageForward are the side keys (left = back, right = forward).
  enum class Button : uint8_t { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  explicit MappedInput(HalGPIO& gpio) : gpio(gpio) {}

  void update();

  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const { return gpio.wasAnyPressed(); }
  bool wasAnyReleased() const { return gpio.wasAnyReleased(); }
  unsigned long powerHeldMs() const { return gpio.getPowerButtonHeldTime(); }

  // Call once from setup() after display.begin().
  static void installBusyWaitPoll();

  void resetPendingPageTaps();

  // Taps queued during a blocking refresh (busy-wait poll). Does not consume.
  // Positive = net forward, negative = net back.
  int queuedPageDelta() const;

  // Net list/page delta from this frame's release edges plus taps drained during a blocking refresh.
  int consumeNavigationDelta();
  // Reader paging. Down/Right forward, Up/Left back. The power button no longer pages
  // (short press now toggles the gyroscope lock; see core/Power.h).
  int consumeReaderPageDelta();

 private:
  HalGPIO& gpio;
  uint8_t map(Button button) const;

  uint16_t consumePendingForwardTaps();
  uint16_t consumePendingBackTaps();
  static uint16_t consumePendingConfirmTaps();
  static uint16_t consumePendingCancelTaps();

  // Latched once per update() so any screen can check Confirm/Back release any
  // number of times this tick and get a consistent answer, including releases
  // that happened mid-blocking-refresh and were recovered via busyWaitPoll.
  bool confirmLatched = false;
  bool backLatched = false;

  // Hook has no instance, so tap counters are static (one MappedInput exists).
  static bool busyWaitPoll(int8_t busyPin, uint8_t busyLevel);
  static uint16_t pendingForwardTaps;
  static uint16_t pendingBackTaps;
  static uint16_t pendingConfirmTaps;
  static uint16_t pendingCancelTaps;
};
