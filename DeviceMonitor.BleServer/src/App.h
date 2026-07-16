#pragma once

#include <stdint.h>

#include "BleMetricsServer.h"
#include "BootButton.h"
#include "ChartView.h"
#include "Display.h"
#include "MonitorModel.h"
#include "TableView.h"
#include "View.h"

// Composition root: owns the hardware, the model and the views, and runs the
// poll-and-render loop. Everything here runs on the main task; the BLE server is the
// only cross-task boundary and is consumed by polling (see BleMetricsServer).
class App {
public:
  void begin();
  void tick();

private:
  static constexpr uint8_t BootButtonPin = 9;  // ESP32-C6 onboard BOOT button
  static constexpr int ViewCount = 3;          // table, CPU chart, GPU chart
  static constexpr uint32_t LoopDelayMs = 20;

  void consumeBleState();
  void render();

  Display _display;
  MonitorModel _model;
  BleMetricsServer _ble;
  BootButton _button{ BootButtonPin };

  TableView _tableView{ _model };
  ChartView _cpuChart{ _model, 0, "CPU" };
  ChartView _gpuChart{ _model, 1, "GPU" };
  View *const _views[ViewCount] = { &_tableView, &_cpuChart, &_gpuChart };

  int _viewIndex = 0;
  uint32_t _lastSeq = 0;
  bool _fullRedraw = false;  // view changed -> repaint the static chrome too
  bool _dirty = false;       // data (or link state) changed -> repaint the content
};
