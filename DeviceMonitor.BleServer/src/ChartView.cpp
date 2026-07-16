#include "ChartView.h"

#include <string.h>

#include "Theme.h"

// Clears the screen and paints the static chrome (title, legend, axes, axis labels)
// plus the current lines.
void ChartView::drawFull(Arduino_GFX &gfx) {
  gfx.fillScreen(Theme::Black);

  // Title (CPU/GPU) top-left in gray, like the table row labels.
  gfx.setTextSize(2);
  gfx.setTextColor(Theme::Gray);
  gfx.setCursor(2, 0);
  gfx.print(_title);

  // Legend: LOAD/TEMP/MEM, each in its line color (matches the table headers).
  gfx.setTextSize(1);
  int lx = 60;
  for (int k = 0; k < 3; k++) {
    gfx.setTextColor(Theme::MetricColors[k]);
    gfx.setCursor(lx, 4);
    gfx.print(Theme::MetricLabels[k]);
    lx += (int)strlen(Theme::MetricLabels[k]) * 6 + 8;
  }

  drawAxes(gfx);
  gfx.setTextSize(1);
  gfx.setTextColor(Theme::Gray);
  gfx.setCursor(PlotL - 18, PlotT - 3);
  gfx.print("100");  // Y max
  gfx.setCursor(PlotL - 6, PlotB - 3);
  gfx.print("0");    // Y min
  gfx.setCursor(PlotR - 6, PlotB + 3);
  gfx.print("t");    // X axis

  for (int k = 0; k < 3; k++) {
    drawMetricLine(gfx, _firstMetric + k);
  }
}

// Clears only the plot interior (right of the Y axis, above the X axis), restores the
// axes and redraws the lines — the chrome painted by drawFull stays.
void ChartView::drawContent(Arduino_GFX &gfx) {
  gfx.fillRect(PlotL + 1, PlotT, PlotR - PlotL, PlotB - PlotT, Theme::Black);
  drawAxes(gfx);
  for (int k = 0; k < 3; k++) {
    drawMetricLine(gfx, _firstMetric + k);
  }
}

void ChartView::drawAxes(Arduino_GFX &gfx) const {
  gfx.drawFastVLine(PlotL, PlotT, PlotB - PlotT + 1, Theme::DarkGray);  // Y axis
  gfx.drawFastHLine(PlotL, PlotB, PlotR - PlotL + 1, Theme::DarkGray);  // X axis
}

// Interpolated polyline over the history samples (oldest at left). Y scale is fixed
// 0..100; Na samples break the line (gap), a lone point after a gap becomes a pixel.
void ChartView::drawMetricLine(Arduino_GFX &gfx, size_t metric) const {
  const MetricsHistory &hist = _model.history;
  if (hist.count() < 2) {
    return;
  }

  const uint16_t color = Theme::MetricColors[metric % 3];
  int prevX = 0, prevY = 0;
  bool havePrev = false;

  for (uint8_t i = 0; i < hist.count(); i++) {
    uint8_t v = hist.at(metric, i);
    if (v == MetricsFrame::Na) {
      havePrev = false;  // gap for missing sample
      continue;
    }
    if (v > 100) {
      v = 100;
    }

    const int x = PlotL + (int)((long)(PlotR - PlotL) * i / (MetricsHistory::Depth - 1));
    const int y = PlotB - (int)((long)(PlotB - PlotT) * v / 100);
    if (havePrev) {
      gfx.drawLine(prevX, prevY, x, y, color);
    } else {
      gfx.drawPixel(x, y, color);
    }
    prevX = x;
    prevY = y;
    havePrev = true;
  }
}
