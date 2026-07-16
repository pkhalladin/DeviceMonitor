#pragma once

// BLE contract — must match the WinUI client exactly (DeviceMonitor.BleClient/BleIds.cs).
namespace BleIds {

inline constexpr const char *DeviceName = "DeviceMonitor";
inline constexpr const char *ServiceUuid = "a1b2c3d4-0001-4a5b-8c6d-1234567890ab";
inline constexpr const char *MetricsUuid = "a1b2c3d4-0002-4a5b-8c6d-1234567890ab";

}  // namespace BleIds
