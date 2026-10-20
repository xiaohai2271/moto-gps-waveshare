#include "wifi_provisioning.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

namespace moto::wifi {

constexpr char kTag[] = "wifi_prov";
constexpr char kNamespace[] = "wifi_cfg";
constexpr char kKeySsid[] = "ssid";
constexpr char kKeyPassword[] = "password";
constexpr char kKeyFlags[] = "flags";

constexpr std::uint8_t kFlagHidden = 1U << 0U;
constexpr std::uint8_t kFlagAutoReconnect = 1U << 1U;

// Limits duplicated from the shared protocol header (wifi constants live in
// the BLE protocol; this module only needs the numeric bounds for buffers).
constexpr std::size_t kMaxWifiSsidLen = 32;
constexpr std::size_t kMaxWifiPasswordLen = 64;

// Exponential reconnect backoff (docs §6.6): 1/2/4/8 s capped at 30 s.
constexpr std::uint32_t kReconnectBaseMs = 1'000;
constexpr std::uint32_t kReconnectMaxMs = 30'000;
constexpr std::size_t kMaxScanResults = 16;

// Internal events routed from the esp_event handler into the module task.
enum class EventType : std::uint8_t {
  StaStart = 0,
  StaDisconnected,
  ScanDone,
  GotIp,
};

constexpr int kEventQueueLength = 8;

struct Event {
  EventType type;
};

// The Impl struct is public within this translation unit so the esp_event
// handler (file-local) can reach it; it stays opaque in the header.
struct WifiProvisioning::Impl {
  StatusCallback callback = nullptr;
  void* callback_context = nullptr;
  ScanDoneCallback scan_callback = nullptr;

  State state = State::Off;
  Error error = Error::None;
  std::int8_t rssi_dbm = 0;
  std::uint32_t ipv4_le = 0;
  std::string ssid;
  Credentials credentials;
  bool has_credentials = false;
  bool temporary_connect = false;  // Do not touch NVS on temporary connects.

  bool scan_running = false;
  std::uint32_t reconnect_attempt = 0;
  esp_timer_handle_t reconnect_timer = nullptr;

  QueueHandle_t events = nullptr;
  TaskHandle_t task = nullptr;
  std::vector<ScanAp> scan_results;

  void publish(State next, Error next_error) {
    state = next;
    error = next_error;
    if (callback != nullptr) {
      Status status;
      status.state = state;
      status.rssi_dbm = rssi_dbm;
      status.error = error;
      status.ipv4_le = ipv4_le;
      status.ssid = ssid;
      callback(status, callback_context);
    }
  }

  void schedule_reconnect() {
    if (reconnect_timer == nullptr || !credentials.auto_reconnect ||
        temporary_connect) {
      return;
    }
    std::uint32_t delay_ms =
        kReconnectBaseMs << std::min<std::uint32_t>(reconnect_attempt, 5U);
    delay_ms = std::min(delay_ms, kReconnectMaxMs);
    reconnect_attempt += 1;
    esp_timer_stop(reconnect_timer);
    esp_timer_start_once(reconnect_timer, delay_ms * 1'000U);
    ESP_LOGI(kTag, "reconnect attempt %u in %u ms",
             static_cast<unsigned>(reconnect_attempt),
             static_cast<unsigned>(delay_ms));
  }
};

namespace {

// IP_EVENT_STA_GOT_IP carries esp_ip4_addr_t; keep the little-endian value.
std::uint32_t ipv4_from_event(const ip_event_got_ip_t* event) {
  return event == nullptr
             ? 0U
             : static_cast<std::uint32_t>(event->ip_info.ip.addr);
}

std::uint8_t auth_to_wire(const wifi_auth_mode_t mode) {
  // Wire enum (docs §5.5): 0 open, 1 wpa2, 2 wpa3, 3 wpa2/wpa3.
  if (mode == WIFI_AUTH_OPEN) {
    return 0;
  }
  if (mode == WIFI_AUTH_WPA3_ENTERPRISE || mode >= WIFI_AUTH_WPA3_PSK) {
    return 2;
  }
  if (mode == WIFI_AUTH_WPA2_WPA3_PSK) {
    return 3;
  }
  return 1;  // WPA/WPA2 personal variants.
}

}  // namespace

namespace {

void reconnect_timer_cb(void* arg) {
  auto* impl = static_cast<WifiProvisioning::Impl*>(arg);
  if (impl->has_credentials && !impl->scan_running) {
    ESP_LOGI(kTag, "reconnect timer fired");
    esp_wifi_connect();
  }
}

void event_handler(void* arg, esp_event_base_t base, std::int32_t id,
                   void* data) {
  auto* impl = static_cast<WifiProvisioning::Impl*>(arg);
  Event event{};
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_START:
        event.type = EventType::StaStart;
        break;
      case WIFI_EVENT_STA_DISCONNECTED:
        event.type = EventType::StaDisconnected;
        break;
      case WIFI_EVENT_SCAN_DONE:
        event.type = EventType::ScanDone;
        break;
      default:
        return;
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    event.type = EventType::GotIp;
    // Carry the IP through the queue payload area (unused otherwise).
    if (data != nullptr) {
      impl->ipv4_le = ipv4_from_event(static_cast<ip_event_got_ip_t*>(data));
    }
  } else {
    return;
  }
  xQueueSend(impl->events, &event, 0);
}

void load_credentials(WifiProvisioning::Impl& impl) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    impl.has_credentials = false;
    return;
  }
  char ssid[kMaxWifiSsidLen + 1] = {};
  char password[kMaxWifiPasswordLen + 1] = {};
  std::size_t ssid_len = sizeof(ssid);
  std::size_t password_len = sizeof(password);
  std::uint8_t flags = 0;
  const bool ok =
      nvs_get_str(handle, kKeySsid, ssid, &ssid_len) == ESP_OK &&
      nvs_get_str(handle, kKeyPassword, password, &password_len) == ESP_OK &&
      nvs_get_u8(handle, kKeyFlags, &flags) == ESP_OK;
  nvs_close(handle);
  if (!ok) {
    impl.has_credentials = false;
    return;
  }
  impl.credentials.ssid = ssid;
  impl.credentials.password = password;
  impl.credentials.hidden = (flags & kFlagHidden) != 0U;
  impl.credentials.auto_reconnect = (flags & kFlagAutoReconnect) != 0U;
  impl.has_credentials = !impl.credentials.ssid.empty();
}

void persist_credentials(const WifiProvisioning::Impl& impl) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(kTag, "could not open %s for write", kNamespace);
    return;
  }
  std::uint8_t flags = 0;
  if (impl.credentials.hidden) {
    flags |= kFlagHidden;
  }
  if (impl.credentials.auto_reconnect) {
    flags |= kFlagAutoReconnect;
  }
  nvs_erase_key(handle, kKeySsid);
  nvs_erase_key(handle, kKeyPassword);
  nvs_erase_key(handle, kKeyFlags);
  nvs_set_str(handle, kKeySsid, impl.credentials.ssid.c_str());
  nvs_set_str(handle, kKeyPassword, impl.credentials.password.c_str());
  nvs_set_u8(handle, kKeyFlags, flags);
  if (nvs_commit(handle) != ESP_OK) {
    ESP_LOGE(kTag, "commit failed");
  }
  nvs_close(handle);
}

void erase_credentials(WifiProvisioning::Impl& impl) {
  impl.has_credentials = false;
  impl.credentials = Credentials{};
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return;
  }
  nvs_erase_key(handle, kKeySsid);
  nvs_erase_key(handle, kKeyPassword);
  nvs_erase_key(handle, kKeyFlags);
  nvs_commit(handle);
  nvs_close(handle);
}

void handle_scan_done(WifiProvisioning::Impl& impl) {
  impl.scan_running = false;
  uint16_t count = 0;
  esp_wifi_scan_get_ap_num(&count);
  count = std::min<std::uint16_t>(count, kMaxScanResults);
  std::vector<wifi_ap_record_t> records(count);
  if (count > 0 &&
      esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
    ESP_LOGW(kTag, "scan fetch failed");
    return;
  }
  impl.scan_results.clear();
  for (std::uint16_t i = 0; i < count; ++i) {
    const wifi_ap_record_t& record = records[i];
    WifiProvisioning::ScanAp ap;
    ap.rssi_dbm = static_cast<std::int8_t>(record.rssi);
    ap.auth = auth_to_wire(record.authmode);
    ap.ssid = reinterpret_cast<const char*>(record.ssid);
    if (!ap.ssid.empty()) {
      impl.scan_results.push_back(std::move(ap));
    }
  }
  ESP_LOGI(kTag, "scan found %u APs",
           static_cast<unsigned>(impl.scan_results.size()));
  if (impl.scan_callback != nullptr) {
    impl.scan_callback(impl.callback_context);
  }
}

void handle_event(WifiProvisioning::Impl& impl, const Event& event) {
  switch (event.type) {
    case EventType::StaStart:
      if (impl.state != State::Connected) {
        impl.publish(State::Connecting, Error::None);
      }
      break;
    case EventType::GotIp:
      impl.publish(State::Connected, Error::None);
      break;
    case EventType::StaDisconnected: {
      impl.ipv4_le = 0;
      if (impl.has_credentials && !impl.temporary_connect) {
        // Map the disconnect reason only loosely: the device keeps trying, so
        // surface "auth" only for the first attempts and timeout afterwards.
        if (impl.reconnect_attempt >= 3) {
          impl.publish(State::Failed, Error::Timeout);
        } else {
          impl.publish(State::Connecting, Error::None);
        }
        impl.schedule_reconnect();
      } else {
        impl.publish(State::Failed, Error::SsidNotFound);
      }
      break;
    }
    case EventType::ScanDone:
      handle_scan_done(impl);
      if (impl.has_credentials) {
        esp_wifi_connect();
      }
      break;
  }
}

void task_body(void* argument) {
  auto* impl = static_cast<WifiProvisioning::Impl*>(argument);
  Event event{};
  for (;;) {
    if (xQueueReceive(impl->events, &event, portMAX_DELAY) == pdTRUE) {
      handle_event(*impl, event);
    }
  }
}

}  // namespace

esp_err_t WifiProvisioning::start(StatusCallback callback, void* context,
                                  ScanDoneCallback scan_callback) {
  if (impl_ != nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  impl_ = new Impl();
  impl_->callback = callback;
  impl_->callback_context = context;
  impl_->scan_callback = scan_callback;

  if (esp_netif_init() != ESP_OK) {
    return ESP_FAIL;
  }
  if (esp_event_loop_create_default() != ESP_OK &&
      esp_event_loop_create_default() != ESP_ERR_INVALID_STATE) {
    return ESP_FAIL;
  }
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&config) != ESP_OK) {
    return ESP_FAIL;
  }
  esp_wifi_set_storage(WIFI_STORAGE_RAM);  // Credentials stay in wifi_cfg.
  esp_wifi_set_mode(WIFI_MODE_NULL);

  impl_->events = xQueueCreate(kEventQueueLength, sizeof(Event));
  if (impl_->events == nullptr) {
    return ESP_FAIL;
  }
  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, impl_));
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, impl_));

  esp_timer_create_args_t timer_args{};
  timer_args.callback = reconnect_timer_cb;
  timer_args.arg = impl_;
  timer_args.name = "wifi_reconnect";
  if (esp_timer_create(&timer_args, &impl_->reconnect_timer) != ESP_OK) {
    return ESP_FAIL;
  }

  if (xTaskCreate(task_body, "wifi_prov", 4'096, impl_, 5,
                  &impl_->task) != pdPASS) {
    return ESP_FAIL;
  }

  load_credentials(*impl_);
  if (impl_->has_credentials) {
    impl_->ssid = impl_->credentials.ssid;
    ESP_LOGI(kTag, "restored credentials for %s",
             impl_->credentials.ssid.c_str());
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
  } else {
    impl_->publish(State::NoCredentials, Error::None);
  }
  return ESP_OK;
}

esp_err_t WifiProvisioning::save_and_connect(const Credentials& credentials) {
  if (impl_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  impl_->credentials = credentials;
  impl_->has_credentials = !credentials.ssid.empty();
  impl_->temporary_connect = false;
  impl_->reconnect_attempt = 0;
  impl_->ssid = credentials.ssid;
  persist_credentials(*impl_);
  impl_->publish(State::Connecting, Error::None);

  esp_wifi_set_mode(WIFI_MODE_STA);
  wifi_config_t wifi_config{};
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
               credentials.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
               credentials.password.c_str(),
               sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  esp_wifi_start();
  esp_wifi_connect();
  return ESP_OK;
}

esp_err_t WifiProvisioning::connect_temporary(const Credentials& credentials) {
  // Same path as save_and_connect but without touching NVS.
  if (impl_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  impl_->credentials = credentials;
  impl_->has_credentials = true;
  impl_->temporary_connect = true;
  impl_->reconnect_attempt = 0;
  impl_->ssid = credentials.ssid;
  impl_->publish(State::Connecting, Error::None);

  esp_wifi_set_mode(WIFI_MODE_STA);
  wifi_config_t wifi_config{};
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
               credentials.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
               credentials.password.c_str(),
               sizeof(wifi_config.sta.password) - 1);
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  esp_wifi_start();
  esp_wifi_connect();
  return ESP_OK;
}

esp_err_t WifiProvisioning::disconnect() {
  if (impl_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (impl_->reconnect_timer != nullptr) {
    esp_timer_stop(impl_->reconnect_timer);
  }
  esp_wifi_disconnect();
  impl_->publish(State::Off, Error::None);
  return ESP_OK;
}

esp_err_t WifiProvisioning::forget() {
  if (impl_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  erase_credentials(*impl_);
  return disconnect();
}

esp_err_t WifiProvisioning::start_scan() {
  if (impl_ == nullptr || impl_->scan_running) {
    return ESP_ERR_INVALID_STATE;
  }
  impl_->scan_running = true;
  // A scan needs the station started even with no credentials.
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  wifi_scan_config_t scan_config{};
  scan_config.show_hidden = true;
  if (esp_wifi_scan_start(&scan_config, false) != ESP_OK) {
    impl_->scan_running = false;
    ESP_LOGW(kTag, "scan start failed");
    return ESP_FAIL;
  }
  return ESP_OK;
}

Status WifiProvisioning::current_status() const {
  Status status;
  if (impl_ == nullptr) {
    return status;
  }
  status.state = impl_->state;
  status.rssi_dbm = impl_->rssi_dbm;
  status.error = impl_->error;
  status.ipv4_le = impl_->ipv4_le;
  status.ssid = impl_->ssid;
  if (impl_->state == State::Connected) {
    wifi_ap_record_t record{};
    if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
      status.rssi_dbm = static_cast<std::int8_t>(record.rssi);
    }
  }
  return status;
}

std::size_t WifiProvisioning::copy_scan_results(ScanAp* out,
                                                std::size_t capacity) const {
  if (impl_ == nullptr || out == nullptr) {
    return 0;
  }
  const std::size_t count =
      std::min(impl_->scan_results.size(), capacity);
  std::copy_n(impl_->scan_results.begin(), count, out);
  return count;
}

bool WifiProvisioning::has_credentials() const {
  return impl_ != nullptr && impl_->has_credentials;
}

}  // namespace moto::wifi
