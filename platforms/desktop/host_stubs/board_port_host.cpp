#include "board_port.h"

#include <chrono>
#include <cstdint>
#include <mutex>

#include "headless_display.h"

namespace {

std::recursive_mutex& board_mutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

}  // namespace

extern "C" int64_t esp_timer_get_time(void) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

extern "C" const char* esp_err_to_name(int code) {
  switch(code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    default: return "ESP_ERR_UNKNOWN";
  }
}

extern "C" esp_err_t board_port_init(void) { return ESP_OK; }

extern "C" lv_display_t* board_port_get_display(void) {
  return moto::sim::display_handle();
}

extern "C" esp_err_t board_port_reveal_display(void) { return ESP_OK; }

extern "C" bool board_port_lock(uint32_t /*timeout_ms*/) {
  board_mutex().lock();
  return true;
}

extern "C" void board_port_unlock(void) { board_mutex().unlock(); }

extern "C" bool board_port_power_button_pressed(void) { return false; }

extern "C" esp_err_t board_port_power_off(void) { return ESP_OK; }
