#include "Display.h"

Display::Display()
    : _bus(PinDc, PinCs, PinSclk, PinMosi, GFX_NOT_DEFINED),
      _panel(&_bus, PinRst, 1 /*rotation*/, true /*IPS*/, 172, 320, 34, 0, 34, 0),
      _canvas(320, 172, &_panel) {}

void Display::begin() {
  // Dim the backlight via PWM (arduino-esp32 3.x LEDC API).
  ledcAttach(PinBacklight, 5000 /*Hz*/, 8 /*bit*/);
  ledcWrite(PinBacklight, BacklightLevel);

  // Inits the panel and allocates the ~108 KB framebuffer.
  if (!_canvas.begin()) {
    Serial.println("Display: canvas framebuffer allocation failed");
  }
}
