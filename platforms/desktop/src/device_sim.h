#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "headless_display.h"
#include "moto_nav_presenter.hpp"
#include "phone_nav_bridge.h"
#include "tcp_link.h"
#include "virtual_ble_transport.h"

namespace moto::sim {

struct DeviceSimOptions {
  std::uint16_t port = 9100;
  std::string frames_dir;
};

// The host virtual device. It owns the real firmware bridge and shared UI and
// advances them on a deterministic 40 Hz frame clock.
class DeviceSim {
 public:
  bool init(const DeviceSimOptions& options, std::string* error);
  void tick(std::uint64_t now_ms);

  bool dump(const std::string& name, std::string* error);

  [[nodiscard]] moto::nav::NavPresenter& presenter() { return presenter_; }
  [[nodiscard]] VirtualBleTransport& transport() { return transport_; }
  [[nodiscard]] HeadlessDisplay& display() { return display_; }
  [[nodiscard]] std::uint16_t port() const { return server_.port(); }
  [[nodiscard]] bool phone_connected() const { return server_.connected(); }
  [[nodiscard]] const moto_ui_layout_t& applied_layout() const;

 private:
  static moto::ble::AckStatus on_message(
      const moto::ble::ReassembledMessage& message, void* context);
  static void on_link(bool active, void* context);

  HeadlessDisplay display_;
  moto::nav::NavPresenter presenter_;
  // Created after the LVGL display and shared UI exist: the bridge constructor
  // restores the persisted layout, which must not touch a null display.
  std::unique_ptr<PhoneNavBridge> bridge_;
  VirtualBleTransport transport_;
  TcpServer server_;

  std::uint64_t last_tick_ms_ = 0;
  bool peer_connected_ = false;
  bool initialised_ = false;
  std::string frames_dir_;
};

}  // namespace moto::sim
