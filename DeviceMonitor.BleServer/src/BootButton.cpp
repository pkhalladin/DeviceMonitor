#include "BootButton.h"

void BootButton::begin() {
  pinMode(_pin, INPUT_PULLUP);
}

bool BootButton::pollPressed() {
  const int raw = digitalRead(_pin);
  const uint32_t now = millis();

  if (raw != _lastRaw) {
    _lastRaw = raw;
    _lastChangeMs = now;
  }

  if ((now - _lastChangeMs) >= _debounceMs && raw != _lastStable) {
    _lastStable = raw;
    return raw == LOW;  // press (HIGH->LOW)
  }

  return false;
}
