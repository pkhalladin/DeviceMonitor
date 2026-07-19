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
//
// A changed value does not snap: the old glyph fades to the background and the new one
// fades out of it, the whole swap lasting max(1, 3 / delta) seconds (mirrors the client
// fade). A glyph that started fading in always lands and rests a second at full
// brightness so the value can be read; changes landing during the fade-in or the rest
// wait as the pending target (last one wins) and fade only after the rest ends.
// Unchanged cells are never repainted. Animations run only while this view is
// active; App cancels them on view/link changes and drawFull snaps to final values.
class TableView : public View {
public:
  explicit TableView(const MonitorModel &model) : _model(model) {}

  void drawFull(Arduino_GFX &gfx) override;
  void drawContent(Arduino_GFX &gfx) override;

  // Starts (or retargets) the per-cell fades for a freshly received frame;
  // prev is the model's previous frame, _model.latest already holds the new one.
  void onFrame(const MetricsFrame &prev, uint32_t nowMs);
  // Drops every running fade without drawing (the caller repaints or the view is hidden).
  void cancelAnimations();
  // Advances every running fade; call each loop tick while this view is active.
  void tickAnimations(Arduino_GFX &gfx, uint32_t nowMs);

private:
  static constexpr int LabelColumnWidth = 48;  // left column holding the CPU/GPU row labels
  static constexpr int HeaderHeight = 34;      // top row holding the LOAD/TEMP/MEM headers

  // One cell's fade: Out shows fromValue dimming to black, In shows toValue
  // brightening, Rest lingers a second at full brightness before the next fade.
  // Timing is millis()-based — the 20 ms loop is too jittery for per-tick
  // stepping to keep real durations.
  struct CellAnim {
    enum Phase : uint8_t { Idle, Out, In, Rest };
    static constexpr uint32_t NoColor = 0xFFFFFFFF;  // forces the first draw of a fade

    Phase phase = Idle;
    uint8_t fromValue = MetricsFrame::Na;
    uint8_t toValue = MetricsFrame::Na;
    uint32_t phaseStartMs = 0;
    uint16_t halfMs = 0;              // one phase = half the total transition
    uint32_t lastColor = NoColor;     // last pushed glyph color — skip no-op redraws
    bool hasPending = false;          // a change arrived during the rest
    uint8_t pendingValue = MetricsFrame::Na;
    uint16_t pendingHalfMs = 0;
  };

  static constexpr uint32_t RestMs = 1000;  // linger after a finished fade

  static int columnWidth(Arduino_GFX &gfx) { return (gfx.width() - LabelColumnWidth) / 3; }
  static int rowHeight(Arduino_GFX &gfx) { return (gfx.height() - HeaderHeight) / 2; }

  // Per-metric change between consecutive frames; a reading appearing or
  // disappearing counts as the maximum move.
  static uint16_t frameDelta(uint8_t prev, uint8_t cur);
  // One fade phase in ms: half of max(1000, 3000 / delta). delta > 0.
  static uint16_t halfMsForDelta(uint16_t delta);
  static void cellRect(Arduino_GFX &gfx, int idx, int &x, int &y, int &w, int &h);

  static void drawHeaderLabel(Arduino_GFX &gfx, int x, int width, const char *text);
  static void drawRowLabel(Arduino_GFX &gfx, int y, int height, const char *text);
  // Final glyph color of a cell (Na/link aware) at the given brightness.
  uint16_t cellColor(uint8_t value, int col, uint8_t level) const;
  void drawCell(Arduino_GFX &gfx, int x, int y, int width, int height,
                uint8_t value, int col, uint8_t level = 255) const;

  const MonitorModel &_model;
  CellAnim _anims[MetricsFrame::Count];
};
