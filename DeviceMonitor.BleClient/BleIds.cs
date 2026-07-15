using System;

namespace DeviceMonitor.BleClient;

/// <summary>
/// BLE contract shared with the ESP32 firmware. These literals must stay in sync
/// with DeviceMonitor.BleServer/src/main.cpp. The metrics characteristic carries a
/// 6-byte frame: [cpuLoad%, cpuTemp°C, ramUsed%, gpuLoad%, gpuTemp°C, vramUsed%],
/// each a uint8 where 255 means "no reading".
/// </summary>
internal static class BleIds
{
    public const string DeviceName = "DeviceMonitor";

    public static readonly Guid ServiceUuid = new("a1b2c3d4-0001-4a5b-8c6d-1234567890ab");

    public static readonly Guid MetricsUuid = new("a1b2c3d4-0002-4a5b-8c6d-1234567890ab");
}
