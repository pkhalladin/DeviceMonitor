#include "Display.h"

Display::Display()
    : _bus(PinDc, PinCs, PinSclk, PinMosi, GFX_NOT_DEFINED),
      _panel(&_bus, PinRst, 1 /*rotation*/, true /*IPS*/, 172, 320, 34, 0, 34, 0) {}

void Display::begin() {
  // Dim the backlight via PWM (arduino-esp32 3.x LEDC API).
  ledcAttach(PinBacklight, 5000 /*Hz*/, 8 /*bit*/);
  ledcWrite(PinBacklight, BacklightLevel);

  _panel.begin();
}
