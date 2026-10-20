#include "device_sim.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "moto_nav_ui.h"
#include "sim_log.h"
#include "wifi_provisioning_host.h"

namespace moto::sim {

bool DeviceSim::init(const DeviceSimOptions& options, std::string* error) {
  if(initialised_) {
    return true;
  }
  log_line("I", "sim", "headless display init");
  if(!display_.init(error)) {
    return false;
  }

  // The shared LVGL UI must exist before the bridge installs its callbacks and
  // restores the persisted layout.
  log_line("I", "sim", "creating shared LVGL UI");
  moto_nav_ui_create();
  log_line("I", "sim", "shared LVGL UI created");
  bridge_ = std::make_unique<PhoneNavBridge>(presenter_);

  transport_.set_callbacks(&DeviceSim::on_message, &DeviceSim::on_link, this);
  transport_.set_send([this](const moto::ble::Bytes& frame) {
    return server_.send_frame(frame);
  });

  bridge_->set_sender(&VirtualBleTransport::send_from_bridge, &transport_);
  bridge_->install_ui_callbacks();
  bridge_->start_wifi();
  bridge_->restore_layout();

  log_line("I", "sim", "starting TCP server");
  if(!server_.listen(options.port, error)) {
    return false;
  }

  initialised_ = true;
  frames_dir_ = options.frames_dir;

  // Settle the boot/connection screen before the first phone frame arrives.
  std::uint64_t now = 0;
  for(int frame = 0; frame < 4; ++frame) {
    now += 25;
    tick(now);
  }
  log_line("I", "sim", "initialised");
  return true;
}

void DeviceSim::tick(std::uint64_t now_ms) {
  if(!initialised_) {
    return;
  }

  std::uint32_t elapsed = 25;
  if(last_tick_ms_ != 0 && now_ms > last_tick_ms_) {
    elapsed = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(now_ms - last_tick_ms_, 50));
  }
  last_tick_ms_ = now_ms;
  display_.advance(elapsed);

  server_.accept_pending();
  if(server_.connected()) {
    if(!peer_connected_) {
      transport_.on_peer_connected();
      peer_connected_ = true;
    }
    std::vector<moto::ble::Bytes> frames;
    if(server_.read_frames(&frames)) {
      for(const auto& frame : frames) {
        transport_.on_frame(frame, now_ms);
      }
    }
    if(!server_.connected()) {
      transport_.on_peer_disconnected();
      peer_connected_ = false;
    }
  } else if(peer_connected_) {
    transport_.on_peer_disconnected();
    peer_connected_ = false;
  }

  wifi::pump(now_ms);
  transport_.tick(now_ms);

  // Mirrors the firmware's single render owner: BLE callbacks only retain
  // state, the frame path applies it to LVGL.
  bridge_->render_pending_for_test();
  lv_timer_handler();
}

bool DeviceSim::dump(const std::string& name, std::string* error) {
  if(frames_dir_.empty()) {
    return true;
  }
  std::error_code code;
  std::filesystem::create_directories(frames_dir_, code);
  if(code) {
    if(error != nullptr) {
      *error = "cannot create frames directory: " + code.message();
    }
    return false;
  }
  return display_.save_bmp(frames_dir_ + "/" + name + ".bmp", error);
}

const moto_ui_layout_t& DeviceSim::applied_layout() const {
  static moto_ui_layout_t fallback{};
  const moto_ui_layout_t* layout = moto_nav_ui_current_layout();
  return layout != nullptr ? *layout : fallback;
}

moto::ble::AckStatus DeviceSim::on_message(
    const moto::ble::ReassembledMessage& message, void* context) {
  auto* self = static_cast<DeviceSim*>(context);
  return self->bridge_->on_message(message);
}

void DeviceSim::on_link(bool active, void* context) {
  auto* self = static_cast<DeviceSim*>(context);
  self->bridge_->on_link_state(active);
}

}  // namespace moto::sim
