#include "MetricsHistory.h"

void MetricsHistory::push(const MetricsFrame &frame) {
  for (size_t m = 0; m < MetricsFrame::Count; m++) {
    _samples[m][_head] = frame.values[m];
  }
  _head = (_head + 1) % Depth;
  if (_count < Depth) {
    _count++;
  }
}

void MetricsHistory::reset() {
  _count = 0;
  _head = 0;
}

uint8_t MetricsHistory::at(size_t metric, uint8_t i) const {
  const int slot = (_head - _count + i + Depth) % Depth;
  return _samples[metric][slot];
}
