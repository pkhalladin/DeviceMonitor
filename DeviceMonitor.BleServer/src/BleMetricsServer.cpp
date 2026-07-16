#include "BleMetricsServer.h"

#include "BleIds.h"

void BleMetricsServer::begin() {
  NimBLEDevice::init(BleIds::DeviceName);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&_serverCallbacks);

  NimBLEService *service = server->createService(BleIds::ServiceUuid);
  NimBLECharacteristic *metrics = service->createCharacteristic(
      BleIds::MetricsUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  metrics->setCallbacks(&_metricsCallbacks);
  // NimBLE 2.x starts services automatically; NimBLEService::start() is a no-op.

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BleIds::ServiceUuid);
  advertising->setName(BleIds::DeviceName);
  advertising->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
}

void BleMetricsServer::copyLatestTo(MetricsFrame &out) const {
  for (size_t i = 0; i < MetricsFrame::Count; i++) {
    out.values[i] = _latest[i];
  }
}

void BleMetricsServer::publish(const MetricsFrame &frame) {
  for (size_t i = 0; i < MetricsFrame::Count; i++) {
    _latest[i] = frame.values[i];
  }
  _seq = _seq + 1;  // values first, then the counter the main task watches
}

void BleMetricsServer::resetLatest() {
  for (size_t i = 0; i < MetricsFrame::Count; i++) {
    _latest[i] = MetricsFrame::Na;
  }
}

void BleMetricsServer::ServerCallbacks::onConnect(NimBLEServer *, NimBLEConnInfo &) {
  _owner._connected = true;
}

void BleMetricsServer::ServerCallbacks::onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) {
  // Drop stale readings so a reconnect shows "--" until fresh data arrives.
  _owner.resetLatest();
  _owner._connected = false;
  NimBLEDevice::startAdvertising();  // keep advertising for the next client
}

void BleMetricsServer::MetricsCallbacks::onWrite(
    NimBLECharacteristic *characteristic, NimBLEConnInfo &) {
  NimBLEAttValue value = characteristic->getValue();

  MetricsFrame frame;
  if (!frame.tryParse(value.data(), value.length())) {
    return;  // too short — ignore the write
  }

  _owner.publish(frame);
}
