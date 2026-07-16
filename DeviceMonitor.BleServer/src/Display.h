#pragma once

#include <Arduino_GFX_Library.h>

// Waveshare ESP32-C6-LCD-1.47 panel: ST7789 172x320 on 4-wire hardware SPI (write-only,
// MISO unused), rotation 1 = landscape 320x172, IPS (color inversion on), column offset
// 34 / row offset 0. Also owns the backlight PWM.
//
// Pin map: MOSI=6  SCLK=7  CS=14  DC=15  RST=21  BLK(backlight)=22
class Display {
public:
  Display();

  void begin();

  Arduino_GFX &gfx() { return _panel; }

private:
  static constexpr int8_t PinMosi = 6;
  static constexpr int8_t PinSclk = 7;
  static constexpr int8_t PinCs = 14;
  static constexpr int8_t PinDc = 15;
  static constexpr int8_t PinRst = 21;
  static constexpr uint8_t PinBacklight = 22;

  // Backlight PWM duty (0–255). Kept low for a dim, minimal glow; raise for a brighter panel.
  static constexpr uint8_t BacklightLevel = 48;

  Arduino_ESP32SPI _bus;
  Arduino_ST7789 _panel;
};
