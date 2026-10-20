#include "wifi_provisioning_host.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "esp_timer.h"
#include "moto/ble_protocol/ble_protocol.hpp"
#include "wifi_health_server.h"
#include "wifi_provisioning.h"

namespace moto::sim::wifi {
namespace {

struct Event {
  enum class Kind { Status, ScanDone };
  std::uint64_t due_ms = 0;
  Kind kind = Kind::Status;
  moto::wifi::Status status{};
};

std::deque<Event>& events() {
  static std::deque<Event> queue;
  return queue;
}

moto::wifi::StatusCallback& status_callback() {
  static moto::wifi::StatusCallback value = nullptr;
  return value;
}

void*& status_context() {
  static void* value = nullptr;
  return value;
}

moto::wifi::ScanDoneCallback& scan_callback() {
  static moto::wifi::ScanDoneCallback value = nullptr;
  return value;
}

void*& scan_context() {
  static void* value = nullptr;
  return value;
}

bool& persisted() {
  static bool value = false;
  return value;
}

moto::wifi::Credentials& saved_credentials() {
  static moto::wifi::Credentials value{};
  return value;
}

}  // namespace

std::uint64_t now_ms_for_events() {
  return static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
}

moto::wifi::Status& latest_status() {
  static moto::wifi::Status value = [] {
    moto::wifi::Status initial;
    initial.state = moto::wifi::State::NoCredentials;
    return initial;
  }();
  return value;
}

void push_status(std::uint64_t due_ms, const moto::wifi::Status& status) {
  Event event;
  event.due_ms = due_ms;
  event.kind = Event::Kind::Status;
  event.status = status;
  events().push_back(event);
}

void push_scan_done(std::uint64_t due_ms) {
  Event event;
  event.due_ms = due_ms;
  event.kind = Event::Kind::ScanDone;
  events().push_back(event);
}

}  // namespace moto::sim::wifi

namespace moto::wifi {
namespace {

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
    return static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
  });
  return value;
}

Status final_status_for(const Credentials& credentials) {
  Status status;
  status.ssid = credentials.ssid;
  const std::string key = lowercase(credentials.ssid);
  if(key.find("fail") != std::string::npos) {
    status.state = State::Failed;
    status.error = Error::Auth;
    return status;
  }
  if(key.find("timeout") != std::string::npos) {
    status.state = State::Failed;
    status.error = Error::Timeout;
    return status;
  }
  status.state = State::Connected;
  status.error = Error::None;
  status.rssi_dbm = -52;
  status.ipv4_le = 0xC0A80402U;
  return status;
}

Status connecting_status_for(const Credentials& credentials) {
  Status status;
  status.state = State::Connecting;
  status.error = Error::None;
  status.ssid = credentials.ssid;
  return status;
}

void schedule_connect(const Credentials& credentials) {
  const std::uint64_t now = moto::sim::wifi::now_ms_for_events();
  moto::sim::wifi::push_status(now, connecting_status_for(credentials));
  moto::sim::wifi::push_status(now + 250U, final_status_for(credentials));
}

WifiProvisioning::ScanAp make_ap(std::int8_t rssi, std::uint8_t auth,
                                 const char* ssid) {
  WifiProvisioning::ScanAp ap;
  ap.rssi_dbm = rssi;
  ap.auth = auth;
  ap.ssid = ssid;
  return ap;
}

}  // namespace

struct WifiProvisioning::Impl {
  StatusCallback callback = nullptr;
  void* callback_context = nullptr;
  ScanDoneCallback scan_callback = nullptr;
  Status status{};
  Credentials credentials{};
  std::vector<ScanAp> scan_results;
};

esp_err_t WifiProvisioning::start(StatusCallback callback, void* context,
                                  ScanDoneCallback scan_cb) {
  if(impl_ != nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  impl_ = new Impl();
  impl_->callback = callback;
  impl_->callback_context = context;
  impl_->scan_callback = scan_cb;

  moto::sim::wifi::status_callback() = callback;
  moto::sim::wifi::status_context() = context;
  moto::sim::wifi::scan_callback() = scan_cb;
  moto::sim::wifi::scan_context() = context;

  if(moto::sim::wifi::persisted()) {
    impl_->credentials = moto::sim::wifi::saved_credentials();
    impl_->status = connecting_status_for(impl_->credentials);
    moto::sim::wifi::latest_status() = impl_->status;
    schedule_connect(impl_->credentials);
  } else {
    impl_->status.state = State::NoCredentials;
    moto::sim::wifi::latest_status() = impl_->status;
  }
  return ESP_OK;
}

esp_err_t WifiProvisioning::save_and_connect(const Credentials& credentials) {
  if(impl_ == nullptr) {
    impl_ = new Impl();
  }
  if(credentials.ssid.empty()) {
    return ESP_ERR_INVALID_ARG;
  }
  impl_->credentials = credentials;
  impl_->status = connecting_status_for(credentials);
  moto::sim::wifi::latest_status() = impl_->status;
  moto::sim::wifi::persisted() = true;
  moto::sim::wifi::saved_credentials() = credentials;
  schedule_connect(credentials);
  return ESP_OK;
}

esp_err_t WifiProvisioning::connect_temporary(const Credentials& credentials) {
  if(impl_ == nullptr) {
    impl_ = new Impl();
  }
  if(credentials.ssid.empty()) {
    return ESP_ERR_INVALID_ARG;
  }
  impl_->credentials = credentials;
  impl_->status = connecting_status_for(credentials);
  moto::sim::wifi::latest_status() = impl_->status;
  schedule_connect(credentials);
  return ESP_OK;
}

esp_err_t WifiProvisioning::disconnect() {
  if(impl_ == nullptr) {
    impl_ = new Impl();
  }
  impl_->status = Status{};
  impl_->status.state = State::Off;
  moto::sim::wifi::latest_status() = impl_->status;
  moto::sim::wifi::push_status(moto::sim::wifi::now_ms_for_events(),
                               impl_->status);
  return ESP_OK;
}

esp_err_t WifiProvisioning::forget() {
  if(impl_ == nullptr) {
    impl_ = new Impl();
  }
  moto::sim::wifi::persisted() = false;
  moto::sim::wifi::saved_credentials() = Credentials{};
  impl_->credentials = Credentials{};
  impl_->scan_results.clear();
  impl_->status = Status{};
  impl_->status.state = State::NoCredentials;
  moto::sim::wifi::latest_status() = impl_->status;
  moto::sim::wifi::push_status(moto::sim::wifi::now_ms_for_events(),
                               impl_->status);
  return ESP_OK;
}

esp_err_t WifiProvisioning::start_scan() {
  if(impl_ == nullptr) {
    impl_ = new Impl();
  }
  if(moto::sim::wifi::latest_status().state == State::Connecting) {
    return ESP_ERR_INVALID_STATE;
  }
  impl_->scan_results.clear();
  impl_->scan_results.push_back(make_ap(-48, static_cast<std::uint8_t>(moto::ble::WifiAuthWpa2), "SIM-HOME"));
  impl_->scan_results.push_back(make_ap(-67, static_cast<std::uint8_t>(moto::ble::WifiAuthOpen), "SIM-GUEST"));
  impl_->scan_results.push_back(make_ap(-79, static_cast<std::uint8_t>(moto::ble::WifiAuthWpa2Wpa3), "SIM-FAIL"));
  moto::sim::wifi::push_scan_done(moto::sim::wifi::now_ms_for_events() + 250U);
  return ESP_OK;
}

Status WifiProvisioning::current_status() const {
  return moto::sim::wifi::latest_status();
}

std::size_t WifiProvisioning::copy_scan_results(ScanAp* out,
                                                std::size_t capacity) const {
  if(impl_ == nullptr || out == nullptr) return 0;
  const std::size_t count = std::min(capacity, impl_->scan_results.size());
  for(std::size_t index = 0; index < count; ++index) {
    out[index] = impl_->scan_results[index];
  }
  return count;
}

bool WifiProvisioning::has_credentials() const {
  return impl_ != nullptr && moto::sim::wifi::persisted();
}

esp_err_t start_health_endpoint() {
  // The virtual device has no IP stack of its own; the health contract is a
  // plain string response and is covered by the gateway tests instead.
  return ESP_OK;
}

void stop_health_endpoint() {}

}  // namespace moto::wifi

namespace moto::sim::wifi {

void pump(std::uint64_t current_ms) {
  while(!events().empty() && events().front().due_ms <= current_ms) {
    const Event event = events().front();
    events().pop_front();
    if(event.kind == Event::Kind::Status) {
      latest_status() = event.status;
      if(status_callback() != nullptr) {
        status_callback()(event.status, status_context());
      }
    } else if(scan_callback() != nullptr) {
      scan_callback()(scan_context());
    }
  }
}

void reset() {
  events().clear();
  persisted() = false;
  saved_credentials() = moto::wifi::Credentials{};
  moto::wifi::Status initial;
  initial.state = moto::wifi::State::NoCredentials;
  latest_status() = initial;
}

}  // namespace moto::sim::wifi
