// Host stubs for the WiFi provisioning module (docs §6.6).
//
// The real wifi_provisioning.cpp / wifi_health_server.cpp depend on ESP-IDF
// headers (esp_wifi, esp_event, esp_http_server, nvs_flash) that do not exist
// in the host test environment. phone_nav_bridge.cpp references their symbols,
// so the host build links against these inert stubs instead.
//
// The WiFi wire protocol itself is covered by ble_protocol_tests (round-trip
// encode/decode of WifiConfig / WifiStatus / WifiScanRequest /
// WifiScanResult); the device-side behaviour is verified on hardware.

#include "wifi_health_server.h"
#include "wifi_provisioning.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace moto::wifi {

struct WifiProvisioning::Impl {
  StatusCallback callback = nullptr;
  void* callback_context = nullptr;
  ScanDoneCallback scan_callback = nullptr;
  Status status{};
  std::vector<ScanAp> scan_results;
};

esp_err_t WifiProvisioning::start(StatusCallback callback, void* context,
                                  ScanDoneCallback scan_callback) {
  if (impl_ != nullptr) return ESP_FAIL;
  impl_ = new Impl();
  impl_->callback = callback;
  impl_->callback_context = context;
  impl_->scan_callback = scan_callback;
  impl_->status.state = State::NoCredentials;
  return ESP_OK;
}

esp_err_t WifiProvisioning::save_and_connect(const Credentials& credentials) {
  if (impl_ == nullptr) impl_ = new Impl();
  if (credentials.ssid.empty()) return ESP_FAIL;
  impl_->status.state = State::Connecting;
  impl_->status.error = Error::None;
  impl_->status.ssid = credentials.ssid;
  return ESP_OK;
}

esp_err_t WifiProvisioning::connect_temporary(const Credentials& credentials) {
  if (impl_ == nullptr) impl_ = new Impl();
  if (credentials.ssid.empty()) return ESP_FAIL;
  impl_->status.state = State::Connecting;
  impl_->status.error = Error::None;
  impl_->status.ssid = credentials.ssid;
  return ESP_OK;
}

esp_err_t WifiProvisioning::disconnect() {
  if (impl_ == nullptr) impl_ = new Impl();
  impl_->status.state = State::Off;
  impl_->status.error = Error::None;
  impl_->status.ipv4_le = 0;
  impl_->status.rssi_dbm = 0;
  impl_->status.ssid.clear();
  return ESP_OK;
}

esp_err_t WifiProvisioning::forget() {
  if (impl_ == nullptr) impl_ = new Impl();
  impl_->status.state = State::NoCredentials;
  impl_->status.error = Error::None;
  impl_->status.ipv4_le = 0;
  impl_->status.rssi_dbm = 0;
  impl_->status.ssid.clear();
  impl_->scan_results.clear();
  return ESP_OK;
}

esp_err_t WifiProvisioning::start_scan() {
  if (impl_ == nullptr) impl_ = new Impl();
  if (impl_->status.state == State::Connecting) return ESP_FAIL;
  impl_->scan_results.clear();
  return ESP_OK;
}

Status WifiProvisioning::current_status() const {
  if (impl_ == nullptr) return Status{};
  return impl_->status;
}

std::size_t WifiProvisioning::copy_scan_results(ScanAp* out,
                                                std::size_t capacity) const {
  if (out == nullptr || impl_ == nullptr) return 0;
  const std::size_t count = std::min(capacity, impl_->scan_results.size());
  for (std::size_t i = 0; i < count; ++i) out[i] = impl_->scan_results[i];
  return count;
}

bool WifiProvisioning::has_credentials() const {
  return impl_ != nullptr && !impl_->status.ssid.empty();
}

esp_err_t start_health_endpoint() { return ESP_OK; }

void stop_health_endpoint() {}

}  // namespace moto::wifi
