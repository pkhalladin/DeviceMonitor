#pragma once

#include <Arduino_GFX_Library.h>

// Waveshare ESP32-C6-LCD-1.47 panel: ST7789 172x320 on 4-wire hardware SPI (write-only,
// MISO unused), rotation 1 = landscape 320x172, IPS (color inversion on), column offset
// 34 / row offset 0. Also owns the backlight PWM.
//
// All drawing goes to a full-screen RGB565 canvas in RAM (320x172x2 = ~108 KB of the
// C6's 512 KB SRAM); flush() pushes the finished frame to the panel in one SPI burst
// (~20 ms at 40 MHz). Nothing draws to the panel directly, so a cleared-then-redrawn
// cell or chart never shows half-painted — no flicker.
//
// Pin map: MOSI=6  SCLK=7  CS=14  DC=15  RST=21  BLK(backlight)=22
class Display {
public:
  Display();

  void begin();

  Arduino_GFX &gfx() { return _canvas; }

  // Pushes the canvas to the panel. Call once per loop tick after drawing;
  // skip it when nothing drew (the panel keeps showing its last frame).
  void flush() { _canvas.flush(); }

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
  Arduino_Canvas _canvas;  // rotation 0: the panel's MADCTL already maps landscape
};
