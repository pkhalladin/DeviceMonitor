#include "App.h"

#include <Arduino.h>

#include "BleIds.h"

void App::begin() {
  Serial.begin(115200);

  _button.begin();
  _display.begin();
  _views[_viewIndex]->drawFull(_display.gfx());  // no client yet -> all "--"

  _ble.begin();
  Serial.print("BLE advertising as ");
  Serial.println(BleIds::DeviceName);
}

void App::tick() {
  if (_button.pollPressed()) {  // BOOT press advances the view
    _viewIndex = (_viewIndex + 1) % ViewCount;
    _fullRedraw = true;
  }

  consumeBleState();
  render();
  delay(LoopDelayMs);
}

// Polls the BLE server into the model — the main-task side of the cross-task boundary,
// so the history ring buffer is never touched concurrently with the BLE host task.
void App::consumeBleState() {
  const uint32_t seq = _ble.frameSeq();
  if (seq != _lastSeq) {
    _lastSeq = seq;
    _ble.copyLatestTo(_model.latest);
    _model.history.push(_model.latest);
    _dirty = true;
  }

  const bool connected = _ble.isConnected();
  if (connected != _model.connected) {
    _model.connected = connected;
    if (!connected) {
      // The server already dropped its stale readings; mirror that in the model and
      // restart the chart history so it rebuilds from zero on reconnect.
      _ble.copyLatestTo(_model.latest);
      _model.history.reset();
    }
    _dirty = true;
  }
}

void App::render() {
  if (_fullRedraw) {  // BOOT pressed: full repaint of the new view
    _fullRedraw = false;
    _dirty = false;
    _views[_viewIndex]->drawFull(_display.gfx());
  } else if (_dirty) {  // fresh data (or link change): update the current view
    _dirty = false;
    _views[_viewIndex]->drawContent(_display.gfx());
  }
}
