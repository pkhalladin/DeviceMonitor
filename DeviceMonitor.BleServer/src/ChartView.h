#pragma once

#include <stddef.h>

#include "MonitorModel.h"
#include "View.h"

// A line chart of one device (CPU or GPU): its three metrics (LOAD/TEMP/MEM) plotted on
// a shared fixed 0..100 scale over the rolling history (~100 s at 1 frame/s, X axis =
// seconds). The plot fills from the left and reaches full width at Depth samples.
class ChartView : public View {
public:
  // chartIndex 0 = CPU (metrics 0..2), 1 = GPU (metrics 3..5).
  ChartView(const MonitorModel &model, int chartIndex, const char *title)
      : _model(model), _firstMetric(chartIndex * 3), _title(title) {}

  void drawFull(Arduino_GFX &gfx) override;
  void drawContent(Arduino_GFX &gfx) override;

private:
  // Plot area (landscape 320x172): left margin for the Y labels, bottom for the X label.
  static constexpr int PlotL = 24;
  static constexpr int PlotT = 20;
  static constexpr int PlotR = 314;
  static constexpr int PlotB = 158;

  void drawAxes(Arduino_GFX &gfx) const;
  void drawMetricLine(Arduino_GFX &gfx, size_t metric) const;

  const MonitorModel &_model;
  const int _firstMetric;
  const char *const _title;
};
