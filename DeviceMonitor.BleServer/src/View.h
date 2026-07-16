#pragma once

#include <Arduino_GFX_Library.h>

// One full-screen view, cycled with the BOOT button. drawFull() paints everything
// including the static chrome (labels, axes, legend); drawContent() repaints only
// the parts that change with data.
class View {
public:
  virtual ~View() = default;

  virtual void drawFull(Arduino_GFX &gfx) = 0;
  virtual void drawContent(Arduino_GFX &gfx) = 0;
};
