#pragma once

#include <stdint.h>

#include <NimBLEDevice.h>

#include "MetricsFrame.h"

// BLE peripheral / GATT server: advertises the DeviceMonitor service and receives the
// metrics frames the WinUI client writes once per second.
//
// NimBLE callbacks run on the BLE host task, so this class only publishes volatile
// bytes plus a frame counter; the main task consumes them by polling the const
// accessors. Each metric is an independent uint8_t, so a cross-task read never sees
// a torn value.
class BleMetricsServer {
public:
  void begin();

  bool isConnected() const { return _connected; }

  // Bumped on every valid frame — lets the main task detect fresh data.
  uint32_t frameSeq() const { return _seq; }

  // Copies the latest received frame (all Na while no client streams data).
  void copyLatestTo(MetricsFrame &out) const;

private:
  class ServerCallbacks : public NimBLEServerCallbacks {
  public:
    explicit ServerCallbacks(BleMetricsServer &owner) : _owner(owner) {}
    void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override;
    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override;

  private:
    BleMetricsServer &_owner;
  };

  class MetricsCallbacks : public NimBLECharacteristicCallbacks {
  public:
    explicit MetricsCallbacks(BleMetricsServer &owner) : _owner(owner) {}
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override;

  private:
    BleMetricsServer &_owner;
  };

  void publish(const MetricsFrame &frame);
  void resetLatest();

  ServerCallbacks _serverCallbacks{ *this };
  MetricsCallbacks _metricsCallbacks{ *this };

  volatile uint8_t _latest[MetricsFrame::Count] = {
    MetricsFrame::Na, MetricsFrame::Na, MetricsFrame::Na,
    MetricsFrame::Na, MetricsFrame::Na, MetricsFrame::Na,
  };
  volatile bool _connected = false;
  volatile uint32_t _seq = 0;
};
