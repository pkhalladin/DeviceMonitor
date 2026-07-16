#pragma once

#include <stdint.h>

#include "MonitorModel.h"
#include "View.h"

// The metrics table (mirrors the client window):
//
//            LOAD%     TEMP °C    MEM%
//     CPU     42         56        48
//     GPU      3         47        24
//
// Units live in the column headers; each cell shows the bare number. Labels are muted
// gray; values are colored per column. There is no textual status line: while no client
// is connected every cell shows a gray "--"; a single value of MetricsFrame::Na is also
// drawn as "--" (that sensor has no reading).
class TableView : public View {
public:
  explicit TableView(const MonitorModel &model) : _model(model) {}

  void drawFull(Arduino_GFX &gfx) override;
  void drawContent(Arduino_GFX &gfx) override;

private:
  static constexpr int LabelColumnWidth = 48;  // left column holding the CPU/GPU row labels
  static constexpr int HeaderHeight = 34;      // top row holding the LOAD/TEMP/MEM headers

  static int columnWidth(Arduino_GFX &gfx) { return (gfx.width() - LabelColumnWidth) / 3; }
  static int rowHeight(Arduino_GFX &gfx) { return (gfx.height() - HeaderHeight) / 2; }

  static void drawHeaderLabel(Arduino_GFX &gfx, int x, int width, const char *text);
  static void drawRowLabel(Arduino_GFX &gfx, int y, int height, const char *text);
  void drawCell(Arduino_GFX &gfx, int x, int y, int width, int height,
                uint8_t value, uint16_t color) const;

  const MonitorModel &_model;
};
