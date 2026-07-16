#pragma once

#include <Arduino.h>

// Debounced edge detector for an active-low push button on INPUT_PULLUP
// (the ESP32-C6 onboard BOOT button).
class BootButton {
public:
  explicit BootButton(uint8_t pin, uint32_t debounceMs = 30)
      : _pin(pin), _debounceMs(debounceMs) {}

  void begin();

  // True exactly once per press (stable HIGH->LOW edge).
  bool pollPressed();

private:
  const uint8_t _pin;
  const uint32_t _debounceMs;
  int _lastRaw = HIGH;
  int _lastStable = HIGH;
  uint32_t _lastChangeMs = 0;
};
