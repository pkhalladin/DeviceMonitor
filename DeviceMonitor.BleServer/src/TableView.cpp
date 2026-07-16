#include "TableView.h"

#include <stdio.h>
#include <string.h>

#include "Theme.h"

// Draws the static frame (column headers, a thin underline, row labels), then the values.
void TableView::drawFull(Arduino_GFX &gfx) {
  gfx.fillScreen(Theme::Black);

  const int cw = columnWidth(gfx);
  drawHeaderLabel(gfx, LabelColumnWidth + 0 * cw, cw, "LOAD%");
  drawHeaderLabel(gfx, LabelColumnWidth + 1 * cw, cw, "TEMP\xF8" "C");  // \xF8 = degree sign
  drawHeaderLabel(gfx, LabelColumnWidth + 2 * cw, cw, "MEM%");
  gfx.drawFastHLine(0, HeaderHeight - 1, gfx.width(), Theme::DarkGray);

  const int rh = rowHeight(gfx);
  drawRowLabel(gfx, HeaderHeight + 0 * rh, rh, "CPU");
  drawRowLabel(gfx, HeaderHeight + 1 * rh, rh, "GPU");

  drawContent(gfx);
}

// Repaints the six value cells. Rows follow the frame layout: CPU = metrics 0..2,
// GPU = metrics 3..5, columns colored LOAD/TEMP/MEM.
void TableView::drawContent(Arduino_GFX &gfx) {
  const int cw = columnWidth(gfx);
  const int rh = rowHeight(gfx);

  for (int row = 0; row < 2; row++) {
    for (int col = 0; col < 3; col++) {
      drawCell(gfx, LabelColumnWidth + col * cw, HeaderHeight + row * rh, cw, rh,
               _model.latest.values[row * 3 + col], Theme::MetricColors[col]);
    }
  }
}

void TableView::drawHeaderLabel(Arduino_GFX &gfx, int x, int width, const char *text) {
  gfx.setTextSize(2);
  gfx.setTextColor(Theme::Gray);
  int16_t x1, y1;
  uint16_t w, h;
  gfx.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  gfx.setCursor(x + (width - (int)w) / 2 - x1, 9);
  gfx.print(text);
}

void TableView::drawRowLabel(Arduino_GFX &gfx, int y, int height, const char *text) {
  gfx.setTextSize(2);
  gfx.setTextColor(Theme::Gray);
  gfx.setCursor(6, y + (height - 16) / 2);
  gfx.print(text);
}

// Paints one value cell: a big colored number, or a gray "--" when disconnected or the
// value is N/A. Units live in the column headers, so the cell shows the bare number only.
void TableView::drawCell(Arduino_GFX &gfx, int x, int y, int width, int height,
                         uint8_t value, uint16_t color) const {
  gfx.fillRect(x, y, width, height, Theme::Black);

  const bool na = !_model.connected || value == MetricsFrame::Na;
  char num[6];
  if (na) {
    strcpy(num, "--");
  } else {
    snprintf(num, sizeof(num), "%u", (unsigned)value);
  }

  const int sz = 4;
  const int numW = (int)strlen(num) * 6 * sz;
  gfx.setTextSize(sz);
  gfx.setTextColor(na ? Theme::Gray : color);
  gfx.setCursor(x + (width - numW) / 2, y + (height - sz * 8) / 2);
  gfx.print(num);
}
