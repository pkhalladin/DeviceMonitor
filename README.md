# DeviceMonitor

A Windows app reads real CPU/GPU metrics and streams them over BLE to an ESP32 board,
which shows them as a table on its LCD.

- **DeviceMonitor.BleClient** — WinUI 3 / .NET 10 desktop app (unpackaged). BLE central /
  GATT client. Reads CPU/GPU load, temperature and memory usage via LibreHardwareMonitorLib
  and writes a 6-byte frame once per second. Left mouse click in the window cycles the
  views: metrics table → CPU chart → GPU chart (~100 s rolling history). While connected
  the charts exactly mirror the device (fed from delivered frames only, restarted from
  zero at connect); while disconnected they fall back to local samples without resetting.
- **DeviceMonitor.BleServer** — PlatformIO firmware for the Waveshare **ESP32-C6-LCD-1.47**.
  BLE peripheral / GATT server: renders the metrics as a landscape 2×3 table on the 1.47"
  ST7789 LCD. The BOOT button cycles the same three views as the client.

## BLE contract (kept in sync on both sides)

| | Value |
|---|---|
| Device name | `DeviceMonitor` |
| Service UUID | `a1b2c3d4-0001-4a5b-8c6d-1234567890ab` |
| Metrics characteristic UUID | `a1b2c3d4-0002-4a5b-8c6d-1234567890ab` (write / write-no-response) |
| Payload | 6 bytes `uint8`: `[cpuLoad%, cpuTemp°C, ramUsed%, gpuLoad%, gpuTemp°C, vramUsed%]` |
| Sentinel | `255` (0xFF) = no reading → shown as `--` |

## Build & run

### Firmware (ESP32-C6)
Needs [PlatformIO](https://platformio.org/) (VS Code extension or the `pio` CLI) and the board on USB.

```
cd DeviceMonitor.BleServer
pio run -t upload
pio device monitor -b 115200
```

The first build downloads the ESP32-C6 toolchain, so it takes a while. On boot the LCD
shows the table (all `--`) and a muted `Advertising` status.

### Client (Windows) — must run elevated
Reading CPU temperature uses a ring0 driver, so the app requests Administrator
(`requireAdministrator` in `app.manifest`). **Run Visual Studio 2026 as Administrator**,
open `DeviceMonitor.sln`, set **DeviceMonitor.BleClient** as the startup project, and press
**F5** (otherwise F5 fails with an elevation error).

The client is a background app: it **connects automatically** (scanning/reconnecting on its
own), lives in the **system tray**, and minimizing or closing the window hides it there.
Left-click / double-click the tray icon to show the window; the tray menu has **Show**,
**Start with Windows** (a Task Scheduler logon task that starts it elevated + minimized),
and **Exit**.

> The solution builds only the WinUI project. The firmware appears as a solution folder
> for convenience but is built/flashed through PlatformIO, not Visual Studio.

## Test end-to-end
Power the board, launch the client → status becomes `Connected` and the LCD table shows the
same CPU/GPU numbers as the app window, updated every second. Sensors without a reading
(e.g. no dedicated GPU) show `--`. If the board is off/out of range the client keeps
scanning and reconnects when it reappears.
