// DeviceMonitor.BleServer — Waveshare ESP32-C6-LCD-1.47
//
// BLE peripheral / GATT server. A BLE central (the WinUI client) writes a 6-byte metrics
// frame once per second; this firmware renders it as a landscape table (like the client):
//
//            LOAD%     TEMP °C    MEM%
//     CPU     42         56        48
//     GPU      3         47        24
//
// Units live in the column headers; each cell shows the bare number. Row/column labels are
// muted gray; values are colored per column (LOAD white, TEMP yellow, MEM violet). There is
// no textual status line: while advertising (no client) every value shows a gray "--"; once
// a client connects and streams data the values appear in color. A single value of 255
// (0xFF) is also drawn as "--" (that sensor has no reading).
//
// Metrics frame (6 x uint8): [cpuLoad%, cpuTemp°C, ramUsed%, gpuLoad%, gpuTemp°C, vramUsed%]
//
// LCD pin map (ESP32-C6-LCD-1.47):
//   MOSI=6  SCLK=7  CS=14  DC=15  RST=21  BLK(backlight)=22
// Panel: ST7789 172x320, column offset 34 / row offset 0, IPS (color inversion on).

#include <Arduino.h>
#include <string.h>
#include <Arduino_GFX_Library.h>
#include <NimBLEDevice.h>

// Recent Arduino_GFX exposes colors as RGB565_*; define the short names we use.
#ifndef BLACK
#define BLACK 0x0000
#endif
#ifndef GRAY
#define GRAY 0x8410
#endif
#ifndef DKGRAY
#define DKGRAY 0x39E7
#endif

// Per-column value colors (RGB565).
#define LOAD_COLOR 0xFFFF  // ~#fefefe (white)
#define TEMP_COLOR 0xFFE0  // yellow
#define MEM_COLOR  0xC01C  // violet (~#C500E6) — pushed toward magenta so it reads purple, not blue

// ---- BLE contract (must match the WinUI client exactly) --------------------
#define DEVICE_NAME  "DeviceMonitor"
#define SERVICE_UUID "a1b2c3d4-0001-4a5b-8c6d-1234567890ab"
#define METRICS_UUID "a1b2c3d4-0002-4a5b-8c6d-1234567890ab"

// ---- LCD pins --------------------------------------------------------------
#define LCD_MOSI 6
#define LCD_SCLK 7
#define LCD_CS   14
#define LCD_DC   15
#define LCD_RST  21
#define LCD_BLK  22
// Backlight PWM duty (0–255). Kept low for a dim, minimal glow; raise for a brighter panel.
#define LCD_BLK_LEVEL 48

// ---- Table geometry (landscape 320x172) ------------------------------------
#define COL0_W   48  // left column holding the CPU/GPU row labels
#define HEADER_H 34  // top row holding the LOAD/TEMP/MEM headers
#define NA 0xFF      // sentinel for "no reading"

// ---- Charts (CPU + GPU line graphs, cycled with the BOOT button) -----------
#define HIST_LEN   100  // rolling history depth: 100 samples = ~100 s (1 frame/s)
#define BOOT_PIN   9    // ESP32-C6 onboard BOOT button (INPUT_PULLUP, pressed = LOW)
#define VIEW_COUNT 3    // view 0 = metrics table, 1 = CPU chart, 2 = GPU chart

// Chart plot area (landscape 320x172): left margin for Y labels, bottom for X label.
#define PLOT_L 24
#define PLOT_T 20
#define PLOT_R 314
#define PLOT_B 158

// 4-wire hardware SPI bus (MISO not used by this write-only panel).
static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED);

// ST7789 172x320, rotation 1 (landscape 320x172), IPS, col/row offsets 34/0.
static Arduino_GFX *gfx =
    new Arduino_ST7789(bus, LCD_RST, 1 /*rotation*/, true /*IPS*/,
                       172, 320, 34, 0, 34, 0);

// ---- Shared state (written from BLE callbacks, read in loop) ----------------
static volatile uint8_t g_cpuLoad = NA, g_cpuTemp = NA, g_ramUsed = NA;
static volatile uint8_t g_gpuLoad = NA, g_gpuTemp = NA, g_vramUsed = NA;
static volatile bool g_connected = false;
static volatile bool g_dirty = true; // values (or connection state) need repaint

// Chart history + view state. Metric order matches the BLE frame and the table columns:
// 0 cpuLoad, 1 cpuTemp, 2 ramUsed, 3 gpuLoad, 4 gpuTemp, 5 vramUsed.
static uint8_t g_hist[6][HIST_LEN];    // rolling per-metric samples (ring buffer)
static uint8_t g_histCount = 0;        // valid samples so far (0..HIST_LEN)
static uint8_t g_histHead = 0;         // slot the next sample is written to
static volatile uint32_t g_seq = 0;    // bumped in onWrite on each real data frame
static uint32_t g_lastSeq = 0;         // last frame consumed (into history) in loop
static int g_view = 0;                 // 0 = table, 1 = CPU chart, 2 = GPU chart
static bool g_viewChanged = false;     // BOOT pressed -> full redraw of the new view

// Two charts (CPU / GPU), each plotting three metric lines on a shared 0..100 scale.
static const char *const CHART_TITLE[2] = { "CPU", "GPU" };
// Line color per metric index (LOAD white, TEMP yellow, MEM violet — as on the table).
static const uint16_t METRIC_COLOR[6] = {
    LOAD_COLOR, TEMP_COLOR, MEM_COLOR,  // CPU: load / temp / ram
    LOAD_COLOR, TEMP_COLOR, MEM_COLOR,  // GPU: load / temp / vram
};
// Shared legend so each colored line is identifiable (matches the table headers).
static const char *const LEGEND_TEXT[3] = { "LOAD", "TEMP", "MEM" };
static const uint16_t LEGEND_COLOR[3] = { LOAD_COLOR, TEMP_COLOR, MEM_COLOR };

// ---- Rendering -------------------------------------------------------------
static int colWidth() { return (gfx->width() - COL0_W) / 3; }
static int rowHeight() { return (gfx->height() - HEADER_H) / 2; }

static void drawHeaderLabel(int cx, int cw, const char *text) {
  gfx->setTextSize(2);
  gfx->setTextColor(GRAY);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(cx + (cw - (int)w) / 2 - x1, 9);
  gfx->print(text);
}

static void drawRowLabel(int cy, int rh, const char *text) {
  gfx->setTextSize(2);
  gfx->setTextColor(GRAY);
  gfx->setCursor(6, cy + (rh - 16) / 2);
  gfx->print(text);
}

// Draws the static frame: column headers, a thin underline, and row labels.
static void drawLabels() {
  const int cw = colWidth();
  const int rh = rowHeight();

  drawHeaderLabel(COL0_W + 0 * cw, cw, "LOAD%");
  drawHeaderLabel(COL0_W + 1 * cw, cw, "TEMP\xF8" "C");  // \xF8 = degree sign
  drawHeaderLabel(COL0_W + 2 * cw, cw, "MEM%");
  gfx->drawFastHLine(0, HEADER_H - 1, gfx->width(), DKGRAY);

  drawRowLabel(HEADER_H + 0 * rh, rh, "CPU");
  drawRowLabel(HEADER_H + 1 * rh, rh, "GPU");
}

// Paints one value cell: a big colored number, or a gray "--" when disconnected or the
// value is N/A. Units live in the column headers, so the cell shows the bare number only.
static void drawCellValue(int cx, int cy, int cw, int ch, uint8_t value,
                          uint16_t color, bool connected) {
  gfx->fillRect(cx, cy, cw, ch, BLACK);

  const bool na = !connected || value == NA;
  char num[6];
  if (na) {
    strcpy(num, "--");
  } else {
    snprintf(num, sizeof(num), "%u", (unsigned)value);
  }

  const uint16_t c = na ? GRAY : color;
  const int sz = 4;
  const int numW = (int)strlen(num) * 6 * sz;
  const int x = cx + (cw - numW) / 2;
  const int y = cy + (ch - sz * 8) / 2;

  gfx->setTextSize(sz);
  gfx->setTextColor(c);
  gfx->setCursor(x, y);
  gfx->print(num);
}

static void drawValues() {
  const int cw = colWidth();
  const int rh = rowHeight();
  const bool conn = g_connected;

  drawCellValue(COL0_W + 0 * cw, HEADER_H + 0 * rh, cw, rh, g_cpuLoad, LOAD_COLOR, conn);
  drawCellValue(COL0_W + 1 * cw, HEADER_H + 0 * rh, cw, rh, g_cpuTemp, TEMP_COLOR, conn);
  drawCellValue(COL0_W + 2 * cw, HEADER_H + 0 * rh, cw, rh, g_ramUsed, MEM_COLOR, conn);

  drawCellValue(COL0_W + 0 * cw, HEADER_H + 1 * rh, cw, rh, g_gpuLoad, LOAD_COLOR, conn);
  drawCellValue(COL0_W + 1 * cw, HEADER_H + 1 * rh, cw, rh, g_gpuTemp, TEMP_COLOR, conn);
  drawCellValue(COL0_W + 2 * cw, HEADER_H + 1 * rh, cw, rh, g_vramUsed, MEM_COLOR, conn);
}

// ---- Charts ----------------------------------------------------------------
// Current value of metric `idx`, in the same order as the BLE frame / table columns.
static uint8_t curValue(int idx) {
  switch (idx) {
    case 0:  return g_cpuLoad;
    case 1:  return g_cpuTemp;
    case 2:  return g_ramUsed;
    case 3:  return g_gpuLoad;
    case 4:  return g_gpuTemp;
    default: return g_vramUsed;
  }
}

// Append the current 6-metric snapshot to the ring buffer. Called only from loop()
// (main task) so the buffer is never touched concurrently with the BLE task.
static void pushHistory() {
  for (int m = 0; m < 6; m++) g_hist[m][g_histHead] = curValue(m);
  g_histHead = (g_histHead + 1) % HIST_LEN;
  if (g_histCount < HIST_LEN) g_histCount++;
}

static void drawChartAxes() {
  gfx->drawFastVLine(PLOT_L, PLOT_T, PLOT_B - PLOT_T + 1, DKGRAY);  // Y axis
  gfx->drawFastHLine(PLOT_L, PLOT_B, PLOT_R - PLOT_L + 1, DKGRAY);  // X axis
}

// Interpolated polyline over the last g_histCount samples (oldest at left). Y scale is
// fixed 0..100; NA samples break the line (gap). Fixed pixel step per sample: the plot
// fills from the left and reaches full width at HIST_LEN samples (X axis = seconds).
static void drawChartLine(int metricIdx) {
  if (g_histCount < 2) return;
  const uint16_t color = METRIC_COLOR[metricIdx];
  int prevX = 0, prevY = 0;
  bool havePrev = false;
  for (int i = 0; i < g_histCount; i++) {
    const int slot = (g_histHead - g_histCount + i + HIST_LEN) % HIST_LEN;
    uint8_t v = g_hist[metricIdx][slot];
    if (v == NA) { havePrev = false; continue; }  // gap for missing sample
    if (v > 100) v = 100;
    const int x = PLOT_L + (int)((long)(PLOT_R - PLOT_L) * i / (HIST_LEN - 1));
    const int y = PLOT_B - (int)((long)(PLOT_B - PLOT_T) * v / 100);
    if (havePrev) gfx->drawLine(prevX, prevY, x, y, color);
    else          gfx->drawPixel(x, y, color);  // lone point after a gap
    prevX = x; prevY = y; havePrev = true;
  }
}

// Draws one chart (chartIdx 0 = CPU, 1 = GPU) with its three metric lines on a shared
// 0..100 scale. full=true clears the screen and paints the static chrome (title, legend,
// axes, axis labels); full=false only clears the plot interior and redraws axes + lines.
static void drawChart(int chartIdx, bool full) {
  const int base = chartIdx * 3;  // first of this chart's three metric indices
  if (full) {
    gfx->fillScreen(BLACK);
    // Title (CPU/GPU) top-left in gray, like the table row labels.
    gfx->setTextSize(2);
    gfx->setTextColor(GRAY);
    gfx->setCursor(2, 0);
    gfx->print(CHART_TITLE[chartIdx]);
    // Legend: LOAD/TEMP/MEM, each in its line color.
    gfx->setTextSize(1);
    int lx = 60;
    for (int k = 0; k < 3; k++) {
      gfx->setTextColor(LEGEND_COLOR[k]);
      gfx->setCursor(lx, 4);
      gfx->print(LEGEND_TEXT[k]);
      lx += (int)strlen(LEGEND_TEXT[k]) * 6 + 8;
    }

    drawChartAxes();
    gfx->setTextSize(1);
    gfx->setTextColor(GRAY);
    gfx->setCursor(PLOT_L - 18, PLOT_T - 3);  gfx->print("100");  // Y max
    gfx->setCursor(PLOT_L - 6, PLOT_B - 3);   gfx->print("0");    // Y min
    gfx->setCursor(PLOT_R - 6, PLOT_B + 3);   gfx->print("t");    // X axis
  } else {
    // Clear only the plot interior (right of the Y axis, above the X axis) then restore axes.
    gfx->fillRect(PLOT_L + 1, PLOT_T, PLOT_R - PLOT_L, PLOT_B - PLOT_T, BLACK);
    drawChartAxes();
  }
  for (int k = 0; k < 3; k++) drawChartLine(base + k);
}

// ---- BOOT button + view dispatch -------------------------------------------
// Debounced edge detect on the active-low BOOT button; advances the view on each press.
static void pollButton() {
  static int lastRaw = HIGH;
  static int lastStable = HIGH;
  static uint32_t lastChangeMs = 0;
  const int raw = digitalRead(BOOT_PIN);
  const uint32_t now = millis();
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeMs = now;
  }
  if ((now - lastChangeMs) >= 30 && raw != lastStable) {
    lastStable = raw;
    if (raw == LOW) {  // press (HIGH->LOW)
      g_view = (g_view + 1) % VIEW_COUNT;
      g_viewChanged = true;
    }
  }
}

// Renders the current view. full=true repaints the static chrome too (on view change).
static void drawView(bool full) {
  if (g_view == 0) {
    if (full) {
      gfx->fillScreen(BLACK);
      drawLabels();
    }
    drawValues();
  } else {
    drawChart(g_view - 1, full);
  }
}

// ---- BLE callbacks ---------------------------------------------------------
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
    g_connected = true;
    g_dirty = true;
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    g_connected = false;
    // Drop stale readings so a reconnect shows "--" until fresh data arrives.
    g_cpuLoad = g_cpuTemp = g_ramUsed = NA;
    g_gpuLoad = g_gpuTemp = g_vramUsed = NA;
    g_histCount = g_histHead = 0;  // reset chart history; rebuilds from zero on reconnect
    g_dirty = true;
    NimBLEDevice::startAdvertising();  // keep advertising for the next client
  }
};

class MetricsCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
    NimBLEAttValue value = characteristic->getValue();
    if (value.length() >= 6) {
      const uint8_t *d = value.data();
      g_cpuLoad = d[0];
      g_cpuTemp = d[1];
      g_ramUsed = d[2];
      g_gpuLoad = d[3];
      g_gpuTemp = d[4];
      g_vramUsed = d[5];
      g_seq++;  // signal loop() to append this frame to the chart history
      g_dirty = true;
    }
  }
};

// ---- Setup / loop ----------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Dim the backlight via PWM (arduino-esp32 3.x LEDC API) to a minimal, low-glare level.
  ledcAttach(LCD_BLK, 5000 /*Hz*/, 8 /*bit*/);
  ledcWrite(LCD_BLK, LCD_BLK_LEVEL);
  pinMode(BOOT_PIN, INPUT_PULLUP);  // BOOT button cycles the view
  gfx->begin();
  gfx->fillScreen(BLACK);
  drawLabels();
  drawValues();  // not connected yet -> all "--"

  NimBLEDevice::init(DEVICE_NAME);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *service = server->createService(SERVICE_UUID);
  NimBLECharacteristic *metrics = service->createCharacteristic(
      METRICS_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  metrics->setCallbacks(new MetricsCallbacks());
  // NimBLE 2.x starts services automatically; NimBLEService::start() is a no-op.

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setName(DEVICE_NAME);
  advertising->enableScanResponse(true);
  NimBLEDevice::startAdvertising();

  Serial.println("BLE advertising as " DEVICE_NAME);
}

void loop() {
  pollButton();

  // Consume any new BLE frame into the chart history (main-task side, no buffer race).
  if (g_seq != g_lastSeq) {
    g_lastSeq = g_seq;
    pushHistory();
    g_dirty = true;
  }

  if (g_viewChanged) {          // BOOT pressed: full repaint of the new view
    g_viewChanged = false;
    drawView(true);
    g_dirty = false;
  } else if (g_dirty) {         // fresh data (or connection change): update current view
    g_dirty = false;
    drawView(false);
  }
  delay(20);
}
