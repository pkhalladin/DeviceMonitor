#pragma once

#include "MetricsFrame.h"
#include "MetricsHistory.h"

// Everything the views render: the latest frame, the link state and the chart history.
// Owned by App and touched only on the main task; views hold a const reference and pull
// from it when drawing.
struct MonitorModel {
  MetricsFrame latest;
  bool connected = false;
  MetricsHistory history;
};
