#pragma once

#include <stdint.h>

#include "MetricsFrame.h"

// Rolling per-metric history feeding the chart views: a ring buffer of the last Depth
// frames (~100 s at 1 frame/s). Owned by the main task only — never touched from the
// BLE host task (see App::consumeBleState).
class MetricsHistory {
public:
  static constexpr uint8_t Depth = 100;

  void push(const MetricsFrame &frame);
  void reset();

  uint8_t count() const { return _count; }

  // Sample of `metric` at position i, 0 = oldest .. count()-1 = newest.
  uint8_t at(size_t metric, uint8_t i) const;

private:
  uint8_t _samples[MetricsFrame::Count][Depth];
  uint8_t _count = 0;  // valid samples so far (0..Depth)
  uint8_t _head = 0;   // slot the next sample is written to
};
