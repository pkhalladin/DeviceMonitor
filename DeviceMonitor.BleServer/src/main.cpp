// DeviceMonitor.BleServer — Waveshare ESP32-C6-LCD-1.47
//
// BLE peripheral showing PC metrics on the onboard LCD. The WinUI client writes a
// 6-byte metrics frame once per second (see MetricsFrame.h); the firmware renders it
// as a table or as CPU/GPU line charts, cycled with the BOOT button. App wires the
// components together; this file only bridges the Arduino entry points.

#include <Arduino.h>

#include "App.h"

static App app;

void setup() { app.begin(); }

void loop() { app.tick(); }
