#include "phone_client.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "demo_fixture/jinan_big_data_demo.hpp"
#include "esp_timer.h"
#include "sim_log.h"

namespace moto::sim {
namespace {

constexpr char kTag[] = "phone";
constexpr std::size_t kFrameSize = 182;

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
}

moto::ble::GeoPointE6 to_e6(const moto::nav::Gcj02Point& point) {
  moto::ble::GeoPointE6 result;
  result.latitude_e6 =
      static_cast<std::int32_t>(std::llround(point.latitude_deg * 1'000'000.0));
  result.longitude_e6 = static_cast<std::int32_t>(
      std::llround(point.longitude_deg * 1'000'000.0));
  return result;
}

}  // namespace

moto::ble::UiLayout make_test_layout(std::uint16_t revision,
                                     std::uint16_t map_height,
                                     std::uint8_t components) {
  moto::ble::UiLayout layout;
  layout.layout_revision = revision;
  layout.flags = moto::ble::UiLayoutNorthUp | moto::ble::UiLayoutMapVisible;
  layout.theme = static_cast<std::uint8_t>(moto::ble::UiThemeId::Dark);
  layout.map_height_px = map_height;
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

  const std::uint8_t count = std::min<std::uint8_t>(components, 8);
  for(std::uint8_t index = 0; index < count; ++index) {
    moto::ble::UiLayoutComponent component;
    component.id = static_cast<std::uint8_t>(index + 1U);
    component.flags = moto::ble::UiComponentEnabled;
    component.size =
        static_cast<std::uint8_t>(moto::ble::UiComponentSize::Medium);
    component.opacity = 100;
    if(component.id == 7U) {
      component.anchor =
          static_cast<std::uint8_t>(moto::ble::UiArcAnchor::Bottom);
      component.span_deg = 180;
    } else if(component.id == 8U) {
      component.anchor =
          static_cast<std::uint8_t>(moto::ble::UiArcAnchor::Left);
      component.span_deg = 120;
    } else {
      component.x = static_cast<std::int16_t>(20 + index * 36);
      component.y = 280;
    }
    layout.components.push_back(component);
  }
  return layout;
}

NavPhoneClient::NavPhoneClient(std::uint16_t port, std::uint32_t session_id)
    : port_(port), session_id_(session_id) {}

NavPhoneClient::~NavPhoneClient() { client_.close(); }

bool NavPhoneClient::fail(const std::string& message) {
  if(error_.empty()) {
    error_ = (step_.empty() ? std::string("phone") : step_) + ": " + message;
  }
  log_line("E", kTag, "%s", error_.c_str());
  return false;
}

bool NavPhoneClient::connect() {
  std::string error;
  if(!client_.connect("127.0.0.1", port_, &error)) {
    return fail(error);
  }
  connected_at_ms_ = now_ms();
  return true;
}

bool NavPhoneClient::begin() {
  step_ = "connect";
  if(!connect()) {
    return false;
  }
  step_ = "handshake";
  return handshake();
}

bool NavPhoneClient::restart(std::uint32_t session_id) {
  session_id_ = session_id;
  sequence_.reset(1);
  inbound_.reset();
  inbox_.clear();
  last_sequence_ = 0;
  session_established_ = false;
  connected_at_ms_ = 0;
  last_heartbeat_ms_ = 0;
  return begin();
}

void NavPhoneClient::drop_link() {
  client_.close();
  session_established_ = false;
}

bool NavPhoneClient::send_message(const moto::ble::Message& message,
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

bool NavPhoneClient::send_raw(moto::ble::MessageType type,
                              moto::ble::ByteView payload,
                              std::uint8_t flags) {
  const std::uint16_t sequence = sequence_.next();
  moto::ble::FramesResult frames = moto::ble::fragment_message(
      type, sequence, payload, kFrameSize, flags);
  if(!frames.ok()) {
    return fail(std::string("raw fragment failed: ") +
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

bool NavPhoneClient::send_status(moto::ble::ConnectionState state) {
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
  status.session_id = session_id_;
  status.max_frame_size = static_cast<std::uint16_t>(kFrameSize);
  status.heartbeat_interval_ms = 1'000;
  return send_message(moto::ble::Message{status}, 0);
}

void NavPhoneClient::maybe_heartbeat() {
  if(!session_established_ || !heartbeat_enabled_) {
    return;
  }
  const std::uint64_t now = now_ms();
  if(now - last_heartbeat_ms_ < 400) {
    return;
  }
  last_heartbeat_ms_ = now;
  moto::ble::Heartbeat heartbeat;
  heartbeat.session_id = session_id_;
  heartbeat.monotonic_ms = static_cast<std::uint32_t>(now - connected_at_ms_);
  heartbeat.status_flags = 0;
  send_message(moto::ble::Message{heartbeat}, 0);
}

bool NavPhoneClient::next_message(moto::ble::Message* message,
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
      return fail("connection closed by device");
    }
    const moto::ble::ReassemblyResult assembled = inbound_.push(
        moto::ble::ByteView(frame.data(), frame.size()), now_ms());
    if(assembled.error != moto::ble::Error::None) {
      log_line("W", kTag, "inbound frame rejected: %s",
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
      log_line("W", kTag, "inbound payload rejected: %s",
               moto::ble::to_string(decoded.error));
      continue;
    }
    *message = decoded.value;
    return true;
  }
  return false;
}

bool NavPhoneClient::expect(moto::ble::MessageType type,
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

bool NavPhoneClient::wait_ack(std::uint16_t sequence, int timeout_ms) {
  moto::ble::AckStatus status = moto::ble::AckStatus::Failed;
  if(!wait_ack_status(sequence, &status, timeout_ms)) {
    return false;
  }
  if(status != moto::ble::AckStatus::Ok &&
     status != moto::ble::AckStatus::Duplicate) {
    return fail("application ACK status " +
                std::to_string(static_cast<int>(status)));
  }
  return true;
}

bool NavPhoneClient::wait_ack_status(std::uint16_t sequence,
                                     moto::ble::AckStatus* status,
                                     int timeout_ms) {
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
        if(status != nullptr) {
          *status = ack->status;
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

bool NavPhoneClient::handshake() {
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
  if(status->session_id != session_id_) {
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

bool NavPhoneClient::send_snapshot(
    const moto::ble::NavigationSnapshot& snapshot, bool ack) {
  if(!send_message(moto::ble::Message{snapshot},
                   ack ? moto::ble::AckRequested : 0)) {
    return false;
  }
  if(ack && !wait_ack(last_sequence_, 2'000)) {
    return fail("no ACK for NavigationSnapshot");
  }
  return true;
}

bool NavPhoneClient::send_geometry_chunks(
    const std::vector<moto::ble::GeoPointE6>& points,
    std::uint32_t route_token, std::uint32_t route_generation,
    std::uint16_t chunk_count) {
  if(points.size() < 2 || chunk_count == 0 ||
     points.size() % chunk_count != 0) {
    return fail("invalid geometry chunk configuration");
  }
  const std::size_t per_chunk = points.size() / chunk_count;

  moto::ble::RouteGeometry geometry;
  geometry.coordinate_system = moto::ble::CoordinateSystem::Gcj02;
  geometry.route_token = route_token;
  geometry.route_generation = route_generation;
  geometry.chunk_count = chunk_count;
  geometry.total_point_count = static_cast<std::uint16_t>(points.size());
  geometry.view_origin = points.front();

  std::uint16_t first_index = 0;
  for(std::uint16_t chunk = 0; chunk < chunk_count; ++chunk) {
    geometry.chunk_index = chunk;
    geometry.first_point_index = first_index;
    geometry.points.assign(
        points.begin() + static_cast<std::ptrdiff_t>(first_index),
        points.begin() +
            static_cast<std::ptrdiff_t>(first_index + per_chunk));
    if(!send_message(moto::ble::Message{geometry},
                     moto::ble::AckRequested)) {
      return false;
    }
    if(!wait_ack(last_sequence_, 2'000)) {
      return fail("no ACK for RouteGeometry chunk " +
                  std::to_string(chunk));
    }
    first_index = static_cast<std::uint16_t>(first_index + per_chunk);
  }
  return true;
}

bool NavPhoneClient::send_layout_variant(std::uint16_t revision,
                                         std::uint16_t map_height,
                                         std::uint8_t components,
                                         moto::ble::AckStatus* status) {
  const moto::ble::UiLayout layout =
      make_test_layout(revision, map_height, components);
  if(!send_message(moto::ble::Message{layout}, moto::ble::AckRequested)) {
    return false;
  }
  moto::ble::AckStatus observed = moto::ble::AckStatus::Failed;
  if(!wait_ack_status(last_sequence_, &observed, 2'000)) {
    return fail("no ACK for UiLayout");
  }
  if(status != nullptr) {
    *status = observed;
  }
  return true;
}

bool NavPhoneClient::await_layout_state(
    std::uint16_t revision,
    const moto::ble::NavigationSnapshot& snapshot, int timeout_ms) {
  const std::uint64_t deadline =
      now_ms() + static_cast<std::uint64_t>(timeout_ms);
  while(now_ms() < deadline) {
    if(!send_snapshot(snapshot, false)) {
      return false;
    }
    moto::ble::Message received;
    if(!next_message(&received, 100)) {
      continue;
    }
    if(moto::ble::message_type(received) ==
       moto::ble::MessageType::UiLayoutState) {
      const auto* state = std::get_if<moto::ble::UiLayoutState>(&received);
      if(state != nullptr && state->layout_revision == revision) {
        return true;
      }
    }
    if(moto::ble::message_type(received) !=
       moto::ble::MessageType::Heartbeat) {
      inbox_.push_back(std::move(received));
    }
  }
  return fail("device never applied layout revision " +
              std::to_string(revision));
}

bool NavPhoneClient::send_map_scene(std::uint32_t revision, int roads,
                                    int points_per_road, int buildings,
                                    int points_per_building,
                                    moto::ble::AckStatus* status) {
  moto::ble::MapScene scene;
  scene.coordinate_system = moto::ble::CoordinateSystem::Gcj02;
  scene.scene_revision = revision;
  scene.radius_m = 500;
  scene.view_origin = to_e6(moto::demo::jinan_big_data::kRoute[17]);

  for(int road_index = 0; road_index < roads; ++road_index) {
    moto::ble::MapRoadPolyline road;
    road.road_class =
        static_cast<moto::ble::MapRoadClass>(road_index % 6);
    for(int point = 0; point < points_per_road; ++point) {
      moto::ble::GeoPointE6 value = scene.view_origin;
      value.latitude_e6 += 30 + road_index * 12 + point * 6;
      value.longitude_e6 += 20 + road_index * 3 + point * 9;
      road.points.push_back(value);
    }
    scene.roads.push_back(std::move(road));
  }

  for(int building_index = 0; building_index < buildings; ++building_index) {
    moto::ble::MapBuildingFootprint building;
    building.building_class = moto::ble::MapBuildingClass::Generic;
    for(int point = 0; point < points_per_building; ++point) {
      moto::ble::GeoPointE6 value = scene.view_origin;
      value.latitude_e6 += 300 + building_index * 10 + point * 5;
      value.longitude_e6 += 200 + building_index * 4 + point * 4;
      building.points.push_back(value);
    }
    scene.buildings.push_back(std::move(building));
  }

  if(!send_message(moto::ble::Message{scene}, moto::ble::AckRequested)) {
    return false;
  }
  moto::ble::AckStatus observed = moto::ble::AckStatus::Failed;
  if(!wait_ack_status(last_sequence_, &observed, 2'000)) {
    return fail("no ACK for MapScene");
  }
  if(status != nullptr) {
    *status = observed;
    return true;
  }
  if(observed != moto::ble::AckStatus::Ok) {
    return fail("MapScene ACK status " +
                std::to_string(static_cast<int>(observed)));
  }
  return true;
}

bool NavPhoneClient::send_max_map_scene(std::uint32_t revision) {
  return send_map_scene(revision, 24, 8, 16, 8);
}

bool NavPhoneClient::send_media() {
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

bool NavPhoneClient::send_wifi_config(const std::string& ssid,
                                      std::uint8_t action, bool ack) {
  moto::ble::WifiConfig config;
  config.action = action;
  config.flags = moto::ble::WifiAutoReconnect;
  config.ssid = ssid;
  if(!ssid.empty()) {
    config.password = "secret123";
  }
  if(!send_message(moto::ble::Message{config},
                   ack ? moto::ble::AckRequested : 0)) {
    return false;
  }
  if(ack && !wait_ack(last_sequence_, 2'000)) {
    return fail("no ACK for WifiConfig");
  }
  return true;
}

bool NavPhoneClient::wait_wifi_state(std::uint8_t state, std::uint8_t* error,
                                     int timeout_ms) {
  const std::uint64_t deadline =
      now_ms() + static_cast<std::uint64_t>(timeout_ms);
  while(now_ms() < deadline) {
    moto::ble::Message received;
    if(!next_message(&received, 100)) {
      continue;
    }
    if(moto::ble::message_type(received) ==
       moto::ble::MessageType::WifiStatus) {
      const auto* status = std::get_if<moto::ble::WifiStatus>(&received);
      if(status != nullptr && status->state == state &&
         (error == nullptr || status->error == *error)) {
        return true;
      }
    }
    if(moto::ble::message_type(received) !=
       moto::ble::MessageType::Heartbeat) {
      inbox_.push_back(std::move(received));
    }
  }
  return fail("WiFi state " + std::to_string(state) + " was not observed");
}

bool NavPhoneClient::inbox_has(moto::ble::MessageType type) const {
  return std::any_of(inbox_.begin(), inbox_.end(), [type](const auto& message) {
    return moto::ble::message_type(message) == type;
  });
}

}  // namespace moto::sim
