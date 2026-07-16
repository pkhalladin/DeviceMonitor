#pragma once

#include <stddef.h>
#include <stdint.h>

// One metrics frame as written by the client over BLE (6 x uint8):
// [cpuLoad%, cpuTemp°C, ramUsed%, gpuLoad%, gpuTemp°C, vramUsed%].
// A value of Na (0xFF) means "no reading" for that sensor.
struct MetricsFrame {
  static constexpr uint8_t Na = 0xFF;
  static constexpr size_t Count = 6;

  uint8_t values[Count] = { Na, Na, Na, Na, Na, Na };

  // Overwrites this frame from a raw BLE payload; false (frame untouched) if too short.
  bool tryParse(const uint8_t *data, size_t length) {
    if (data == nullptr || length < Count) {
      return false;
    }
    for (size_t i = 0; i < Count; i++) {
      values[i] = data[i];
    }
    return true;
  }
};
