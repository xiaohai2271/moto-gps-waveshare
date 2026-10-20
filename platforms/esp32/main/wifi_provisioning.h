#pragma once

// WiFi provisioning for the device (docs/NAV_LAYOUT_EDITOR_PLAN.md §5.5/§6.6).
//
// Owns the ESP32-S3 WiFi STA lifecycle: credentials arriving over BLE are
// stored in NVS ("wifi_cfg") and applied to the station. The module reports
// state changes through a callback so the bridge can emit WifiStatus messages
// without knowing anything about esp_wifi itself.
//
// Thread model: all entry points are safe to call from the BLE worker task;
// they post onto an internal event queue processed by the module's own task.
// The status callback fires from that task, never from a NimBLE callback.

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace moto::wifi {

// Mirrors the wire enum in shared/ble_protocol (WifiState).
enum class State : std::uint8_t {
  Off = 0,
  Connecting = 1,
  Connected = 2,
  Failed = 3,
  NoCredentials = 4,
};

// Mirrors WifiError on the wire.
enum class Error : std::uint8_t {
  None = 0,
  Auth = 1,
  SsidNotFound = 2,
  Timeout = 3,
  Internal = 4,
};

struct Status {
  State state = State::Off;
  std::int8_t rssi_dbm = 0;
  Error error = Error::None;
  std::uint32_t ipv4_le = 0;
  std::string ssid;
};

struct Credentials {
  std::string ssid;
  std::string password;
  bool hidden = false;
  bool auto_reconnect = true;
};

using StatusCallback = void (*)(const Status& status, void* context);
// Fired once per completed scan; the bridge forwards the results to the phone.
using ScanDoneCallback = void (*)(void* context);

class WifiProvisioning {
 public:
  // Initializes esp_netif/esp_wifi (station only), loads the persisted
  // credentials and, when auto_reconnect is set, starts connecting. Safe to
  // call once at boot; repeated calls return ESP_ERR_INVALID_STATE.
  esp_err_t start(StatusCallback callback, void* context,
                  ScanDoneCallback scan_callback = nullptr);

  // Saves credentials to NVS ("wifi_cfg") and connects now. An empty SSID
  // with action=disconnect stops the station; forget erases the stored
  // record. Called from the BLE worker.
  esp_err_t save_and_connect(const Credentials& credentials);
  esp_err_t connect_temporary(const Credentials& credentials);
  esp_err_t disconnect();
  esp_err_t forget();

  // Best-effort scan; results arrive through on_scan_done once the scan
  // completes. Returns ESP_ERR_INVALID_STATE while a scan is already
  // running or the station is connecting.
  esp_err_t start_scan();

  // Latest status snapshot (rssi/ip refreshed on each event).
  Status current_status() const;

  // Scan results from the most recent completed scan; empty when none.
  struct ScanAp {
    std::int8_t rssi_dbm;
    std::uint8_t auth;
    std::string ssid;
  };
  std::size_t copy_scan_results(ScanAp* out, std::size_t capacity) const;

  // Whether credentials exist in NVS (any namespace record, valid or not).
  bool has_credentials() const;

  // Defined in the .cpp; opaque to callers. Kept public only because the
  // esp_event handler in the same translation unit needs the complete type.
  struct Impl;

 private:
  Impl* impl_ = nullptr;
};

}  // namespace moto::wifi
