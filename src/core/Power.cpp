#include "core/Power.h"

#include <Arduino.h>
#include <Gfx.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "core/ScreenManager.h"
#include "core/Settings.h"

extern Gfx gfx;

namespace {
unsigned long allowSleepAt = 0;
unsigned long lastActivity = 0;
bool wakePowerReleasePending = false;
bool powerReleasedSinceWake = false;
bool pendingPowerOff = false;
bool tiltLock = false;
constexpr unsigned long kShortPressMs = 800;

bool powerHoldAllowed() { return powerReleasedSinceWake && millis() >= allowSleepAt; }

bool isLongPowerHold(const HalGPIO& gpio) {
  return gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() > kShortPressMs;
}

bool isLongPowerRelease(const HalGPIO& gpio) {
  return gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() > kShortPressMs;
}

void paintDeepSleepWhite() {
  // Physical white regardless of night mode: inversion is applied on the
  // way to the panel, so turn it off before filling 0xFF.
  display.setInverted(false);
  display.clearScreen(0xFF);
  display.displayBuffer(HalDisplay::FULL_REFRESH);
}

int iosPtToPx(const int pt) {
  // iOS point: 1pt = 1/163 inch. X4 Classic 4.26" 800×480 ≈ 219 PPI.
  const int ppi = 219;
  return (pt * ppi + 163 / 2) / 163;
}

void paintReaderCornerDots(const bool gyroOff, const bool powerOff) {
  // Leave the current page on the panel. Resync the gray baseline so FAST
  // only drives the markers instead of promoting to a flashing HALF.
  gfx.cleanupGrayscaleBuffers();
  const int dot = iosPtToPx(8);
  if (gyroOff) {
    gfx.fillRect(1, 1, dot, dot, true);
  }
  if (powerOff) {
    gfx.fillRect(gfx.width() - dot - 1, 1, dot, dot, true);
  }
  gfx.present(HalDisplay::FAST_REFRESH);
}

void enterDeepSleep(HalGPIO& gpio) {
  // Sleep prep (panel refresh, GPIO13 latch) must not run at the 10 MHz idle clock.
  HalPowerManager::Lock powerLock;
  if (screenManager.isReader()) {
    paintReaderCornerDots(tiltLock, true);
  } else {
    paintDeepSleepWhite();
  }
  halTiltSensor.deepSleep();
  display.deepSleep();
  powerManager.startDeepSleep(gpio);
}

void setTiltLock(const bool on) {
  if (tiltLock == on) {
    return;
  }
  tiltLock = on;
  if (on) {
    halTiltSensor.deepSleep();
    LOG_INF("SLP", "Gyro lock on");
    if (screenManager.isReader()) {
      paintReaderCornerDots(true, false);
    }
  } else {
    LOG_INF("SLP", "Gyro lock off");
    if (screenManager.isReader()) {
      screenManager.requestUpdate();
    }
  }
}
}  // namespace

void power::noteWakeHold() {
  wakePowerReleasePending = true;
  powerReleasedSinceWake = false;
  pendingPowerOff = false;
  allowSleepAt = millis() + 2000;
  lastActivity = millis();
  LOG_DBG("SLP", "Wake hold, sleep allowed in 2s");
}

bool power::isWakeReleasePending() { return wakePowerReleasePending; }

bool power::consumeWakeRelease(HalGPIO& gpio) {
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    powerReleasedSinceWake = true;
    pendingPowerOff = false;
    return true;
  }
  return false;
}

void power::noteUserActivity(HalGPIO& gpio) {
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity()) {
    lastActivity = millis();
    powerManager.setPowerSaving(false);
  }
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
    powerReleasedSinceWake = true;
  }
}

void power::pollForHold(HalGPIO& gpio) {
  if (powerHoldAllowed() && isLongPowerHold(gpio)) {
    pendingPowerOff = true;
  }
}

bool power::maybeSleep(HalGPIO& gpio, const Settings& settings) {
  if (lastActivity == 0) {
    lastActivity = millis();
  }
  // blockSleep() only gates the idle timer (file transfer, firmware flash).
  if (powerHoldAllowed() && (pendingPowerOff || isLongPowerHold(gpio) || isLongPowerRelease(gpio))) {
    pendingPowerOff = false;
    LOG_INF("SLP", "Power hold, sleeping");
    enterDeepSleep(gpio);
    return true;
  }

  if (screenManager.blocksSleep()) {
    return false;
  }

  const unsigned long idleMs = millis() - lastActivity;
  const unsigned long trueMs = settings.trueSleepTimeoutMs();
  if (trueMs > 0 && idleMs >= trueMs) {
    LOG_INF("SLP", "Idle timeout, deep sleep");
    enterDeepSleep(gpio);
    return true;
  }
  const unsigned long gyroMs = settings.gyroAutoOffTimeoutMs();
  if (gyroMs > 0 && !tiltLock && settings.tiltPageTurn && idleMs >= gyroMs) {
    LOG_INF("SLP", "Idle timeout, gyro lock");
    setTiltLock(true);
  }
  return false;
}

bool power::tiltLocked() { return tiltLock; }

void power::paintGyroOffMarker() { paintReaderCornerDots(true, false); }

bool power::maybeToggleTiltLock(HalGPIO& gpio) {
  // The wake-hold release (finger still down right after waking from deep
  // sleep) must not also toggle the gyro lock.
  if (wakePowerReleasePending) {
    return false;
  }

  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() <= kShortPressMs) {
    setTiltLock(!tiltLock);
    return true;
  }

  if (tiltLock && (gpio.wasAnyPressed() || gpio.wasAnyReleased())) {
    setTiltLock(false);
  }
  return false;
}

void power::idleDelay() {
  // The idle power-saving delay only matters on the reader screen, where the
  // device typically sits for long stretches between button presses. Other
  // screens (home, settings, browser, file transfer) are short, active
  // interactions and shouldn't be throttled — the idle sleep timeout in
  // maybeSleep() still applies regardless.
  if (!screenManager.isReader()) {
    delay(1);
    return;
  }
  if (millis() - lastActivity >= HalPowerManager::IDLE_POWER_SAVING_MS) {
    powerManager.setPowerSaving(true);
    delay(50);
  } else {
    delay(10);
  }
}
