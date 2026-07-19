#pragma once

#include <stdint.h>

// RGB565 palette shared by the views (matches the WinUI client colors).
namespace Theme {

inline constexpr uint16_t Black = 0x0000;
inline constexpr uint16_t Gray = 0x8410;      // muted labels and "--" cells
inline constexpr uint16_t DarkGray = 0x39E7;  // separator line, chart axes

// Per-column value colors: LOAD white, TEMP yellow, MEM violet (~#C500E6 — pushed
// toward magenta so it reads purple, not blue).
inline constexpr uint16_t Load = 0xFFFF;
inline constexpr uint16_t Temp = 0xFFE0;
inline constexpr uint16_t Mem = 0xC01C;

// Color and legend label of metric column k (0 LOAD, 1 TEMP, 2 MEM) — the CPU and GPU
// rows of the frame use the same triple.
inline constexpr uint16_t MetricColors[3] = { Load, Temp, Mem };
inline constexpr const char *MetricLabels[3] = { "LOAD", "TEMP", "MEM" };

// Glyph color at brightness level 0..255 against the black background: scale the
// 5-6-5 channels (the panel has no alpha, so a fade is a plain color ramp; if the
// background ever stops being black this becomes a two-color lerp per channel).
inline uint16_t Fade(uint16_t color, uint8_t level) {
  const uint16_t r = (color >> 11) & 0x1F;
  const uint16_t g = (color >> 5) & 0x3F;
  const uint16_t b = color & 0x1F;
  return (uint16_t)((((r * level) / 255) << 11) |
                    (((g * level) / 255) << 5) |
                    ((b * level) / 255));
}

}  // namespace Theme
