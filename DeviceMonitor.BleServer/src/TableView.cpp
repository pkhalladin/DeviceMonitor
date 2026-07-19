#include "TableView.h"

#include <stdio.h>
#include <string.h>

#include "Theme.h"

// Draws the static frame (column headers, a thin underline, row labels), then the values.
void TableView::drawFull(Arduino_GFX &gfx) {
  cancelAnimations();  // a full repaint always lands on final values
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
               _model.latest.values[row * 3 + col], col);
    }
  }
}

// ---- Delta-driven cell fades (mirrors the client's tray/cell animation) ----------

uint16_t TableView::frameDelta(uint8_t prev, uint8_t cur) {
  if (prev == cur) {
    return 0;
  }
  if (prev == MetricsFrame::Na || cur == MetricsFrame::Na) {
    return 255;
  }
  return prev > cur ? prev - cur : cur - prev;
}

uint16_t TableView::halfMsForDelta(uint16_t delta) {
  const uint32_t totalMs = delta >= 3 ? 1000 : 3000 / delta;  // max(1 s, 3 s / delta)
  return (uint16_t)(totalMs / 2);
}

void TableView::cellRect(Arduino_GFX &gfx, int idx, int &x, int &y, int &w, int &h) {
  w = columnWidth(gfx);
  h = rowHeight(gfx);
  x = LabelColumnWidth + (idx % 3) * w;
  y = HeaderHeight + (idx / 3) * h;
}

void TableView::onFrame(const MetricsFrame &prev, uint32_t nowMs) {
  for (size_t m = 0; m < MetricsFrame::Count; m++) {
    const uint8_t cur = _model.latest.values[m];
    const uint16_t delta = frameDelta(prev.values[m], cur);
    if (delta == 0) {
      continue;  // unchanged cells are never repainted
    }

    const uint16_t half = halfMsForDelta(delta);
    CellAnim &anim = _anims[m];
    if (anim.phase == CellAnim::Idle) {
      anim.fromValue = prev.values[m];
      anim.toValue = cur;
      anim.halfMs = half;
      anim.phaseStartMs = nowMs;
      anim.phase = CellAnim::Out;
      anim.lastColor = CellAnim::NoColor;
      anim.hasPending = false;
      continue;
    }

    // Fading in or resting: the incoming glyph always lands and rests a full
    // second before it may leave — only record the pending target (last wins).
    if (anim.phase != CellAnim::Out) {
      anim.hasPending = true;
      anim.pendingValue = cur;
      anim.pendingHalfMs = half;
      continue;
    }

    // Still fading out — the incoming glyph has not shown yet. Keep the outgoing
    // glyph's brightness at the new delta's pace (back-dated start) and either
    // swap the incoming glyph, or — if the target bounced back to the glyph
    // still on screen — brighten it back instead of a same-value dip.
    const uint32_t elapsed = nowMs - anim.phaseStartMs;
    const uint32_t progress = elapsed >= anim.halfMs ? 255 : elapsed * 255 / anim.halfMs;
    anim.toValue = cur;
    anim.halfMs = half;
    if (cur == anim.fromValue) {
      anim.phase = CellAnim::In;
      anim.phaseStartMs = nowMs - (255 - progress) * half / 255;
    } else {
      anim.phaseStartMs = nowMs - progress * half / 255;
    }
  }
}

void TableView::cancelAnimations() {
  for (size_t m = 0; m < MetricsFrame::Count; m++) {
    _anims[m].phase = CellAnim::Idle;
    _anims[m].hasPending = false;
  }
}

void TableView::tickAnimations(Arduino_GFX &gfx, uint32_t nowMs) {
  for (size_t m = 0; m < MetricsFrame::Count; m++) {
    CellAnim &anim = _anims[m];
    if (anim.phase == CellAnim::Idle) {
      continue;
    }

    // Resting: hold the finished value; when the rest ends, either fade toward
    // the pending target or go idle.
    if (anim.phase == CellAnim::Rest) {
      if (nowMs - anim.phaseStartMs < RestMs) {
        continue;
      }
      if (!anim.hasPending || anim.pendingValue == anim.toValue) {
        anim.phase = CellAnim::Idle;
        continue;
      }
      anim.fromValue = anim.toValue;
      anim.toValue = anim.pendingValue;
      anim.halfMs = anim.pendingHalfMs;
      anim.hasPending = false;
      anim.phase = CellAnim::Out;
      anim.phaseStartMs = nowMs;
    }

    uint32_t elapsed = nowMs - anim.phaseStartMs;
    if (anim.phase == CellAnim::Out && elapsed >= anim.halfMs) {
      anim.phase = CellAnim::In;  // bottom of the dip — the glyph swaps here
      anim.phaseStartMs = nowMs;
      elapsed = 0;
    }

    uint8_t value;
    uint8_t level;
    bool done = false;
    if (anim.phase == CellAnim::Out) {
      value = anim.fromValue;
      level = (uint8_t)(255 - elapsed * 255 / anim.halfMs);
    } else if (elapsed >= anim.halfMs) {
      value = anim.toValue;
      level = 255;
      done = true;
    } else {
      value = anim.toValue;
      level = (uint8_t)(elapsed * 255 / anim.halfMs);
    }

    const uint16_t color = cellColor(value, (int)(m % 3), level);
    if (done) {
      // Linger so the value can be read; a pending target recorded during the
      // fade-in survives into the rest and is applied when it ends.
      anim.phase = CellAnim::Rest;
      anim.phaseStartMs = nowMs;
    }
    if (color == anim.lastColor) {
      continue;  // quantized to the same RGB565 — skip the SPI churn
    }

    anim.lastColor = color;
    int x, y, w, h;
    cellRect(gfx, (int)m, x, y, w, h);
    drawCell(gfx, x, y, w, h, value, (int)(m % 3), level);
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

uint16_t TableView::cellColor(uint8_t value, int col, uint8_t level) const {
  const bool na = !_model.connected || value == MetricsFrame::Na;
  return Theme::Fade(na ? Theme::Gray : Theme::MetricColors[col], level);
}

// Paints one value cell: a big colored number, or a gray "--" when disconnected or the
// value is N/A. Units live in the column headers, so the cell shows the bare number only.
void TableView::drawCell(Arduino_GFX &gfx, int x, int y, int width, int height,
                         uint8_t value, int col, uint8_t level) const {
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
  gfx.setTextColor(cellColor(value, col, level));
  gfx.setCursor(x + (width - numW) / 2, y + (height - sz * 8) / 2);
  gfx.print(num);
}
