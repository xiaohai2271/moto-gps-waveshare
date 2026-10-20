#include "selftest_phone.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "demo_fixture/jinan_big_data_demo.hpp"
#include "device_sim.h"
#include "esp_timer.h"
#include "moto_nav_ui.h"
#include "sim_log.h"

namespace moto::sim {
namespace {

constexpr char kStepTag[] = "selftest";
constexpr std::uint16_t kSimSessionId = 0x51A7U;
constexpr std::size_t kFrameSize = 182;

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
}

void sleep_ms(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

moto::ble::GeoPointE6 to_e6(const moto::nav::Gcj02Point& point) {
  moto::ble::GeoPointE6 result;
  result.latitude_e6 =
      static_cast<std::int32_t>(std::llround(point.latitude_deg * 1'000'000.0));
  result.longitude_e6 = static_cast<std::int32_t>(
      std::llround(point.longitude_deg * 1'000'000.0));
  return result;
}

class ScriptedPhone {
 public:
  explicit ScriptedPhone(std::uint16_t port) : port_(port) {}
  ~ScriptedPhone() { client_.close(); }

  void run();
  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  bool fail(const std::string& message);
  bool connect();
  bool send_message(const moto::ble::Message& message, std::uint8_t flags);
  bool send_status(moto::ble::ConnectionState state);
  void maybe_heartbeat();
  bool next_message(moto::ble::Message* message, int timeout_ms);
  bool expect(moto::ble::MessageType type, moto::ble::Message* message,
              int timeout_ms);
  bool wait_ack(std::uint16_t sequence, int timeout_ms);
  bool handshake();
  bool send_navigation();
  bool send_geometry();
  bool send_map_scene();
  bool send_media();
  bool send_layout();
  bool exercise_wifi();
  bool inbox_has(moto::ble::MessageType type) const;

  std::uint16_t port_ = 0;
  TcpClient client_;
  moto::ble::Reassembler inbound_{};
  moto::ble::SequenceGenerator sequence_{1};
  std::uint64_t connected_at_ms_ = 0;
  std::uint64_t last_heartbeat_ms_ = 0;
  std::uint16_t last_sequence_ = 0;
  bool session_established_ = false;
  std::vector<moto::ble::Message> inbox_;
  std::string step_;
  std::string error_;
};

bool ScriptedPhone::fail(const std::string& message) {
  if(error_.empty()) {
    error_ = (step_.empty() ? std::string("selftest") : step_) + ": " + message;
  }
  log_line("E", kStepTag, "%s", error_.c_str());
  return false;
}

bool ScriptedPhone::connect() {
  std::string error;
  if(!client_.connect("127.0.0.1", port_, &error)) {
    return fail(error);
  }
  connected_at_ms_ = now_ms();
  log_line("I", kStepTag, "phone connected to 127.0.0.1:%u",
           static_cast<unsigned>(port_));
  return true;
}

bool ScriptedPhone::send_message(const moto::ble::Message& message,
                                 std::uint8_t flags) {
  const moto::ble::BytesResult encoded = moto::ble::encode_message(message);
  if(!encoded.ok()) {
    return fail(std::string("encode failed: ") +
                moto::ble::to_string(encoded.error));
  }
  const std::uint16_t sequence = sequence_.next();
  moto::ble::FramesResult frames = moto::ble::fragment_message(
      moto::ble::message_type(message), sequence,
      moto::ble::ByteView(encoded.value), kFrameSize, flags);
  if(!frames.ok()) {
    return fail(std::string("fragment failed: ") +
                moto::ble::to_string(frames.error));
  }
  for(const auto& frame : frames.value) {
    if(!client_.send_frame(frame)) {
      return fail("socket write failed");
    }
  }
  last_sequence_ = sequence;
  return true;
}

bool ScriptedPhone::send_status(moto::ble::ConnectionState state) {
  moto::ble::ConnectionStatus status;
  status.role = moto::ble::EndpointRole::Phone;
  status.state = state;
  status.minimum_version = moto::ble::kProtocolVersion;
  status.maximum_version = moto::ble::kProtocolVersion;
  status.capabilities = moto::ble::CapabilityNavigation |
                        moto::ble::CapabilityRouteGeometry |
                        moto::ble::CapabilityTraffic |
                        moto::ble::CapabilityMediaState |
                        moto::ble::CapabilityTouchCommands |
                        moto::ble::CapabilityMusicCommands |
                        moto::ble::CapabilityCommandAck |
                        moto::ble::CapabilityMapScene |
                        moto::ble::CapabilityUiLayout |
                        moto::ble::CapabilityWifiProvisioning;
  status.session_id = kSimSessionId;
  status.max_frame_size = static_cast<std::uint16_t>(kFrameSize);
  status.heartbeat_interval_ms = 1'000;
  return send_message(moto::ble::Message{status}, 0);
}

void ScriptedPhone::maybe_heartbeat() {
  if(!session_established_) {
    return;
  }
  const std::uint64_t now = now_ms();
  if(now - last_heartbeat_ms_ < 400) {
    return;
  }
  last_heartbeat_ms_ = now;
  moto::ble::Heartbeat heartbeat;
  heartbeat.session_id = kSimSessionId;
  heartbeat.monotonic_ms = static_cast<std::uint32_t>(now - connected_at_ms_);
  heartbeat.status_flags = 0;
  send_message(moto::ble::Message{heartbeat}, 0);
}

bool ScriptedPhone::next_message(moto::ble::Message* message,
                                 int timeout_ms) {
  const std::uint64_t deadline =
      now_ms() + static_cast<std::uint64_t>(std::max(timeout_ms, 1));
  while(now_ms() < deadline) {
    maybe_heartbeat();
    const int remaining =
        static_cast<int>(deadline - now_ms()) > 0
            ? static_cast<int>(deadline - now_ms())
            : 1;
    moto::ble::Bytes frame;
    const int result = client_.read_frame(&frame, remaining);
    if(result == 0) {
      return false;
    }
    if(result < 0) {
      fail("connection closed by device");
      return false;
    }
    const moto::ble::ReassemblyResult assembled = inbound_.push(
        moto::ble::ByteView(frame.data(), frame.size()), now_ms());
    if(assembled.error != moto::ble::Error::None) {
      log_line("W", kStepTag, "inbound frame rejected: %s",
               moto::ble::to_string(assembled.error));
      continue;
    }
    if(!assembled.complete()) {
      continue;
    }
    const moto::ble::MessageResult decoded = moto::ble::decode_message(
        assembled.message.type,
        moto::ble::ByteView(assembled.message.payload));
    if(!decoded.ok()) {
      log_line("W", kStepTag, "inbound payload rejected: %s",
               moto::ble::to_string(decoded.error));
      continue;
    }
    *message = decoded.value;
    return true;
  }
  return false;
}

bool ScriptedPhone::expect(moto::ble::MessageType type,
                           moto::ble::Message* message, int timeout_ms) {
  const std::uint64_t deadline =
      now_ms() + static_cast<std::uint64_t>(timeout_ms);
  while(now_ms() < deadline) {
    moto::ble::Message received;
    const int remaining = static_cast<int>(deadline - now_ms());
    if(!next_message(&received, std::max(remaining, 1))) {
      return false;
    }
    if(moto::ble::message_type(received) == type) {
      *message = std::move(received);
      return true;
    }
    if(moto::ble::message_type(received) !=
       moto::ble::MessageType::Heartbeat) {
      inbox_.push_back(std::move(received));
    }
  }
  return false;
}

bool ScriptedPhone::wait_ack(std::uint16_t sequence, int timeout_ms) {
  const std::uint64_t deadline =
      now_ms() + static_cast<std::uint64_t>(timeout_ms);
  while(now_ms() < deadline) {
    moto::ble::Message received;
    const int remaining = static_cast<int>(deadline - now_ms());
    if(!next_message(&received, std::max(remaining, 1))) {
      return false;
    }
    if(moto::ble::message_type(received) == moto::ble::MessageType::Ack) {
      const auto* ack = std::get_if<moto::ble::Ack>(&received);
      if(ack != nullptr && ack->acknowledged_sequence == sequence) {
        if(ack->status != moto::ble::AckStatus::Ok &&
           ack->status != moto::ble::AckStatus::Duplicate) {
          return fail("application ACK status " +
                      std::to_string(static_cast<int>(ack->status)));
        }
        return true;
      }
    }
    if(moto::ble::message_type(received) !=
       moto::ble::MessageType::Heartbeat) {
      inbox_.push_back(std::move(received));
    }
  }
  return false;
}

bool ScriptedPhone::inbox_has(moto::ble::MessageType type) const {
  return std::any_of(inbox_.begin(), inbox_.end(), [type](const auto& message) {
    return moto::ble::message_type(message) == type;
  });
}

bool ScriptedPhone::handshake() {
  if(!send_status(moto::ble::ConnectionState::Starting)) {
    return false;
  }
  session_established_ = true;
  last_heartbeat_ms_ = now_ms();

  moto::ble::Message message;
  if(!expect(moto::ble::MessageType::ConnectionStatus, &message, 2'000)) {
    return fail("no device ConnectionStatus after Phone Starting");
  }
  auto* status = std::get_if<moto::ble::ConnectionStatus>(&message);
  if(status == nullptr || status->role != moto::ble::EndpointRole::Device) {
    return fail("device status has the wrong role");
  }
  if(status->state != moto::ble::ConnectionState::Ready) {
    return fail("device did not report Ready");
  }
  if(status->session_id != kSimSessionId) {
    return fail("device did not echo the phone session id");
  }
  if((status->capabilities & moto::ble::CapabilityUiLayout) == 0U) {
    return fail("device did not advertise the UI layout capability");
  }

  if(!send_status(moto::ble::ConnectionState::Ready)) {
    return false;
  }
  if(!expect(moto::ble::MessageType::ConnectionStatus, &message, 2'000)) {
    return fail("no final Device Ready");
  }
  status = std::get_if<moto::ble::ConnectionStatus>(&message);
  if(status == nullptr || status->state != moto::ble::ConnectionState::Ready) {
    return fail("final device status is not Ready");
  }

  // The bridge emits UiLayoutState and WifiStatus on the Ready transition.
  const std::uint64_t deadline = now_ms() + 1'500;
  while(now_ms() < deadline &&
        !(inbox_has(moto::ble::MessageType::UiLayoutState) &&
          inbox_has(moto::ble::MessageType::WifiStatus))) {
    moto::ble::Message extra;
    if(!next_message(&extra, 100)) {
      continue;
    }
    if(moto::ble::message_type(extra) != moto::ble::MessageType::Heartbeat) {
      inbox_.push_back(std::move(extra));
    }
  }
  if(!inbox_has(moto::ble::MessageType::UiLayoutState)) {
    return fail("device did not send UiLayoutState after Ready");
  }
  if(!inbox_has(moto::ble::MessageType::WifiStatus)) {
    return fail("device did not send WifiStatus after Ready");
  }
  return true;
}

moto::ble::NavigationSnapshot make_snapshot() {
  moto::ble::NavigationSnapshot snapshot;
  snapshot.state = moto::ble::NavigationState::Navigating;
  snapshot.network = moto::ble::NetworkState::Online;
  snapshot.display_page = moto::ble::DisplayPage::Navigation;
  snapshot.maneuver = moto::ble::Maneuver::Right;
  snapshot.traffic = moto::ble::TrafficLevel::Slow;
  snapshot.flags = moto::ble::NavigationHasDestination |
                   moto::ble::NavigationHasFix |
                   moto::ble::NavigationHasNextManeuver |
                   moto::ble::NavigationHasRouteView;
  snapshot.route_token =
      moto::ble::route_token(moto::demo::jinan_big_data::kRouteId);
  snapshot.route_generation = 3;
  snapshot.maneuver_id = 1;
  snapshot.distance_to_maneuver_m = 120;
  snapshot.remaining_distance_m = 1'500;
  snapshot.remaining_duration_s = 200;
  snapshot.route_progress_m = 400;
  snapshot.total_distance_m = 2'000;
  snapshot.speed_deci_kph = 720;
  snapshot.speed_limit_kph = 70;
  snapshot.heading_cdeg = 9'000;
  snapshot.accuracy_dm = 50;
  snapshot.cross_track_dm = 0;
  snapshot.roundabout_exit = 0;
  snapshot.road_name = "泉城广场北路";
  snapshot.instruction = "右转进入泉城路";
  return snapshot;
}

bool ScriptedPhone::send_navigation() {
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  if(!send_message(moto::ble::Message{snapshot}, moto::ble::AckRequested)) {
    return false;
  }
  if(!wait_ack(last_sequence_, 2'000)) {
    return fail("no ACK for NavigationSnapshot");
  }
  return send_geometry();
}

bool ScriptedPhone::send_geometry() {
  const auto& route = moto::demo::jinan_big_data::kRoute;
  // The rider sits near the bottom of the map viewport, so the visible route
  // has to extend north of the origin. Walk the fixture backwards from its
  // 18th point to keep the whole chunk window on screen.
  constexpr std::size_t kFirstPoint = 17;
  moto::ble::RouteGeometry geometry;
  geometry.coordinate_system = moto::ble::CoordinateSystem::Gcj02;
  geometry.route_token =
      moto::ble::route_token(moto::demo::jinan_big_data::kRouteId);
  geometry.route_generation = 3;
  geometry.chunk_count = 3;
  geometry.total_point_count = 18;
  geometry.view_origin = to_e6(route[kFirstPoint]);

  std::uint16_t first_index = 0;
  for(std::uint16_t chunk = 0; chunk < 3; ++chunk) {
    geometry.chunk_index = chunk;
    geometry.first_point_index = first_index;
    geometry.points.clear();
    for(std::size_t index = 0; index < 6; ++index) {
      geometry.points.push_back(
          to_e6(route[kFirstPoint - (chunk * 6 + index)]));
    }
    first_index = static_cast<std::uint16_t>(first_index + 6);
    if(!send_message(moto::ble::Message{geometry},
                     moto::ble::AckRequested)) {
      return false;
    }
    if(!wait_ack(last_sequence_, 2'000)) {
      return fail("no ACK for RouteGeometry chunk " + std::to_string(chunk));
    }
  }

  // The next snapshot is the presentation boundary for the completed route
  // view, exactly like the phone's 5 Hz cadence.
  return send_message(moto::ble::Message{make_snapshot()}, 0);
}

bool ScriptedPhone::send_map_scene() {
  moto::ble::MapScene scene;
  scene.coordinate_system = moto::ble::CoordinateSystem::Gcj02;
  scene.scene_revision = 7;
  scene.radius_m = 300;
  scene.view_origin = to_e6(moto::demo::jinan_big_data::kRoute.front());

  for(int road_index = 0; road_index < 2; ++road_index) {
    moto::ble::MapRoadPolyline road;
    road.road_class = road_index == 0 ? moto::ble::MapRoadClass::Primary
                                      : moto::ble::MapRoadClass::Residential;
    for(int point = 0; point < 4; ++point) {
      moto::ble::GeoPointE6 value = scene.view_origin;
      value.latitude_e6 += (point + road_index) * 60;
      value.longitude_e6 += point * 40;
      road.points.push_back(value);
    }
    scene.roads.push_back(std::move(road));
  }

  moto::ble::MapBuildingFootprint building;
  building.building_class = moto::ble::MapBuildingClass::Generic;
  for(int point = 0; point < 5; ++point) {
    moto::ble::GeoPointE6 value = scene.view_origin;
    value.latitude_e6 += 100 + point * 30;
    value.longitude_e6 += 100 + point * 20;
    building.points.push_back(value);
  }
  scene.buildings.push_back(std::move(building));

  if(!send_message(moto::ble::Message{scene}, moto::ble::AckRequested)) {
    return false;
  }
  if(!wait_ack(last_sequence_, 2'000)) {
    return fail("no ACK for MapScene");
  }
  return true;
}

bool ScriptedPhone::send_media() {
  moto::ble::MediaState media;
  media.flags = moto::ble::MediaConnected | moto::ble::MediaPlaying;
  media.track_token = 9;
  media.position_s = 42;
  media.duration_s = 210;
  media.source_name = "SIM Music";
  media.track_title = "Hardware Free";
  media.artist_name = "Virtual Device";
  return send_message(moto::ble::Message{media}, 0);
}

bool ScriptedPhone::send_layout() {
  moto::ble::UiLayout layout;
  layout.layout_revision = 3;
  layout.flags = moto::ble::UiLayoutNorthUp | moto::ble::UiLayoutMapVisible;
  layout.theme = static_cast<std::uint8_t>(moto::ble::UiThemeId::Dark);
  layout.map_height_px = 232;
  layout.map_opacity = 100;
  layout.route_r = 255;
  layout.route_g = 255;
  layout.route_b = 255;
  layout.route_width_px = 8;
  layout.route_opacity = 100;
  layout.street_r = 140;
  layout.street_g = 140;
  layout.street_b = 148;
  layout.street_width_px = 4;
  layout.street_opacity = 80;
  layout.building_opacity = 40;
  layout.layer_mask = moto::ble::UiLayerRoads | moto::ble::UiLayerBuildings |
                      moto::ble::UiLayerRoute;
  layout.zoom_percent = 100;

  moto::ble::UiLayoutComponent speed;
  speed.id = static_cast<std::uint8_t>(moto::ble::UiComponentId::Speed);
  speed.flags = moto::ble::UiComponentEnabled;
  speed.size = static_cast<std::uint8_t>(moto::ble::UiComponentSize::Medium);
  speed.opacity = 100;
  speed.x = 60;
  speed.y = 300;
  layout.components.push_back(speed);

  moto::ble::UiLayoutComponent maneuver;
  maneuver.id = static_cast<std::uint8_t>(moto::ble::UiComponentId::Maneuver);
  maneuver.flags = moto::ble::UiComponentEnabled;
  maneuver.size = static_cast<std::uint8_t>(moto::ble::UiComponentSize::Large);
  maneuver.opacity = 100;
  maneuver.x = 200;
  maneuver.y = 330;
  layout.components.push_back(maneuver);

  moto::ble::UiLayoutComponent progress;
  progress.id =
      static_cast<std::uint8_t>(moto::ble::UiComponentId::ProgressArc);
  progress.flags = moto::ble::UiComponentEnabled;
  progress.anchor = static_cast<std::uint8_t>(moto::ble::UiArcAnchor::Bottom);
  progress.span_deg = 180;
  progress.opacity = 100;
  layout.components.push_back(progress);

  moto::ble::UiLayoutComponent speed_arc;
  speed_arc.id = static_cast<std::uint8_t>(moto::ble::UiComponentId::SpeedArc);
  speed_arc.flags = moto::ble::UiComponentEnabled;
  speed_arc.anchor = static_cast<std::uint8_t>(moto::ble::UiArcAnchor::Left);
  speed_arc.span_deg = 120;
  speed_arc.opacity = 100;
  layout.components.push_back(speed_arc);

  if(!send_message(moto::ble::Message{layout}, moto::ble::AckRequested)) {
    return false;
  }
  if(!wait_ack(last_sequence_, 2'000)) {
    return fail("no ACK for UiLayout");
  }

  // The bridge applies a pushed layout after a 2 s debounce on the navigation
  // render path, so keep feeding snapshots until UiLayoutState reports it.
  const std::uint64_t deadline = now_ms() + 4'000;
  while(now_ms() < deadline) {
    if(!send_message(moto::ble::Message{make_snapshot()}, 0)) {
      return false;
    }
    moto::ble::Message received;
    if(!next_message(&received, 100)) {
      continue;
    }
    if(moto::ble::message_type(received) ==
       moto::ble::MessageType::UiLayoutState) {
      const auto* state = std::get_if<moto::ble::UiLayoutState>(&received);
      if(state != nullptr && state->layout_revision == 3) {
        return true;
      }
    }
    if(moto::ble::message_type(received) !=
       moto::ble::MessageType::Heartbeat) {
      inbox_.push_back(std::move(received));
    }
  }
  return fail("device never applied layout revision 3");
}

bool ScriptedPhone::exercise_wifi() {
  moto::ble::WifiScanRequest request;
  request.request_id = 42;
  if(!send_message(moto::ble::Message{request}, 0)) {
    return false;
  }
  moto::ble::Message received;
  if(!expect(moto::ble::MessageType::WifiScanResult, &received, 2'000)) {
    return fail("no WifiScanResult");
  }
  const auto* scan = std::get_if<moto::ble::WifiScanResult>(&received);
  if(scan == nullptr || scan->request_id != 42 ||
     scan->access_points.size() != 3) {
    return fail("WifiScanResult did not echo request 42 with three APs");
  }

  moto::ble::WifiConfig config;
  config.action = moto::ble::WifiSaveAndConnect;
  config.flags = moto::ble::WifiAutoReconnect;
  config.ssid = "SIM-NET";
  config.password = "secret123";
  if(!send_message(moto::ble::Message{config}, moto::ble::AckRequested)) {
    return false;
  }
  if(!wait_ack(last_sequence_, 2'000)) {
    return fail("no ACK for WifiConfig");
  }

  bool connected = false;
  const std::uint64_t deadline = now_ms() + 2'000;
  while(now_ms() < deadline && !connected) {
    moto::ble::Message status_message;
    if(!next_message(&status_message, 100)) {
      continue;
    }
    if(moto::ble::message_type(status_message) ==
       moto::ble::MessageType::WifiStatus) {
      const auto* status = std::get_if<moto::ble::WifiStatus>(&status_message);
      if(status != nullptr && status->state == moto::ble::WifiConnected &&
         status->ssid == "SIM-NET") {
        connected = true;
        break;
      }
    }
    if(moto::ble::message_type(status_message) !=
       moto::ble::MessageType::Heartbeat) {
      inbox_.push_back(std::move(status_message));
    }
  }
  if(!connected) {
    return fail("device never reported WiFi connected");
  }

  config.ssid = "SIM-FAIL";
  if(!send_message(moto::ble::Message{config}, moto::ble::AckRequested)) {
    return false;
  }
  if(!wait_ack(last_sequence_, 2'000)) {
    return fail("no ACK for failing WifiConfig");
  }

  bool failed = false;
  const std::uint64_t fail_deadline = now_ms() + 2'000;
  while(now_ms() < fail_deadline && !failed) {
    moto::ble::Message status_message;
    if(!next_message(&status_message, 100)) {
      continue;
    }
    if(moto::ble::message_type(status_message) ==
       moto::ble::MessageType::WifiStatus) {
      const auto* status = std::get_if<moto::ble::WifiStatus>(&status_message);
      if(status != nullptr && status->state == moto::ble::WifiFailed &&
         status->error == moto::ble::WifiErrorAuth) {
        failed = true;
        break;
      }
    }
    if(moto::ble::message_type(status_message) !=
       moto::ble::MessageType::Heartbeat) {
      inbox_.push_back(std::move(status_message));
    }
  }
  if(!failed) {
    return fail("device never reported the simulated WiFi auth failure");
  }
  return true;
}

void ScriptedPhone::run() {
  step_ = "connect";
  if(!connect()) {
    return;
  }
  step_ = "handshake";
  if(!handshake()) {
    return;
  }
  step_ = "navigation";
  if(!send_navigation()) {
    return;
  }
  step_ = "map-scene";
  if(!send_map_scene()) {
    return;
  }
  step_ = "media";
  if(!send_media()) {
    return;
  }
  step_ = "layout";
  if(!send_layout()) {
    return;
  }
  step_ = "wifi";
  if(!exercise_wifi()) {
    return;
  }
  step_ = "final";
  if(!send_message(moto::ble::Message{make_snapshot()}, 0)) {
    return;
  }
  sleep_ms(150);
  log_line("I", kStepTag, "script completed");
}

bool check_ui(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  const auto require = [error](bool condition, const char* message) {
    if(!condition) {
      if(error != nullptr) *error = message;
      return false;
    }
    return true;
  };

  if(!require(state.mode == MOTO_UI_NAVIGATING,
              "device UI is not in the navigating mode")) {
    return false;
  }
  if(!require(state.maneuver == MOTO_MANEUVER_RIGHT,
              "device UI did not map the right-turn maneuver")) {
    return false;
  }
  if(!require(state.distance_to_maneuver_m == 120,
              "device UI did not receive the maneuver distance")) {
    return false;
  }
  if(!require(state.road_name != nullptr &&
                  std::string(state.road_name) == "泉城广场北路",
              "device UI did not receive the road name")) {
    return false;
  }
  if(!require(state.route_point_count > 0,
              "device UI did not receive the route view geometry")) {
    return false;
  }
  if(!require(state.page == MOTO_UI_PAGE_NAVIGATION,
              "device UI page is not navigation")) {
    return false;
  }
  if(!require(state.online == 1, "device UI did not mark the phone online")) {
    return false;
  }

  const moto_ui_layout_t& layout = sim.applied_layout();
  if(!require(layout.component_count == 4,
              "device did not apply the pushed component layout")) {
    return false;
  }
  if(!require(layout.map_height_px == 232,
              "device did not apply the pushed map height")) {
    return false;
  }
  return true;
}

}  // namespace

bool run_selftest(DeviceSim& sim, bool dump_frames, std::string* error) {
  const std::uint64_t initial_hash = sim.display().frame_hash();
  if(dump_frames) {
    std::string dump_error;
    if(!sim.dump("01-offline", &dump_error)) {
      if(error != nullptr) *error = dump_error;
      return false;
    }
  }

  std::atomic<bool> finished{false};
  std::atomic<bool> release{false};
  std::string script_error;
  std::thread worker([&] {
    ScriptedPhone phone(sim.port());
    phone.run();
    script_error = phone.error();
    finished.store(true);
    // Keep the phone session alive until the main thread has inspected the
    // device, so checks happen while the link is still up.
    while(!release.load(std::memory_order_acquire)) {
      sleep_ms(2);
    }
  });

  const std::uint64_t deadline = now_ms() + 20'000;
  while(!finished.load() && now_ms() < deadline) {
    sim.tick(now_ms());
    sleep_ms(5);
  }
  const bool timed_out = !finished.load();

  // Present the last accepted state while the phone link is still connected.
  if(!timed_out) {
    for(int frame = 0; frame < 4; ++frame) {
      sim.tick(now_ms());
      sleep_ms(5);
    }
  }

  std::string failure;
  bool ok = true;
  if(timed_out) {
    ok = false;
    failure = "self-test timed out";
  } else if(!script_error.empty()) {
    ok = false;
    failure = script_error;
  } else if(!sim.transport().protocol_ready()) {
    ok = false;
    failure = "transport never reached protocol Ready";
  } else if(!check_ui(sim, &failure)) {
    ok = false;
  } else if(initial_hash == sim.display().frame_hash()) {
    ok = false;
    failure = "the framebuffer did not change after the navigation snapshot";
  } else if(dump_frames && !sim.dump("02-navigation", &failure)) {
    ok = false;
  }

  release.store(true, std::memory_order_release);
  worker.join();

  if(!ok) {
    if(error != nullptr) *error = failure;
    return false;
  }
  return true;
}

}  // namespace moto::sim
