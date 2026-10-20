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
#include "phone_client.h"
#include "sim_log.h"

namespace moto::sim {
namespace {

constexpr char kTag[] = "case";

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

std::vector<moto::ble::GeoPointE6> visible_route_points() {
  const auto& route = moto::demo::jinan_big_data::kRoute;
  std::vector<moto::ble::GeoPointE6> points;
  points.reserve(18);
  // The rider is near the bottom of the map viewport, so walk the fixture
  // backwards to keep the projected route on screen.
  for(std::size_t index = 0; index <= 17; ++index) {
    points.push_back(to_e6(route[17 - index]));
  }
  return points;
}

enum class CaseKind {
  Baseline,
  SessionReconnect,
  LayoutRules,
  GeometryMismatch,
  GeometryRecovery,
  WifiErrors,
  StateAcquiring,
  StateArrived,
  StateOffline,
  StateRerouting,
  MapSceneStale,
  WatchdogOffline,
  WatchdogRecovery,
  Fragmentation,
};

void script_baseline(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  if(!phone.send_snapshot(snapshot, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(),
                                 snapshot.route_token,
                                 snapshot.route_generation, 3)) {
    return;
  }
  if(!phone.send_snapshot(snapshot, false)) return;
  if(!phone.send_map_scene(7, 2, 4, 1, 5)) return;
  if(!phone.send_media()) return;

  moto::ble::AckStatus status = moto::ble::AckStatus::Failed;
  if(!phone.send_layout_variant(3, 232, 4, &status)) return;
  if(status != moto::ble::AckStatus::Ok) {
    phone.fail("valid layout revision 3 was rejected");
    return;
  }
  if(!phone.await_layout_state(3, snapshot, 4'000)) return;

  if(!phone.send_wifi_config("SIM-NET", moto::ble::WifiSaveAndConnect, true)) {
    return;
  }
  if(!phone.wait_wifi_state(moto::ble::WifiConnected, nullptr, 2'000)) return;

  if(!phone.send_wifi_config("SIM-FAIL", moto::ble::WifiSaveAndConnect,
                             true)) {
    return;
  }
  std::uint8_t auth_error = moto::ble::WifiErrorAuth;
  if(!phone.wait_wifi_state(moto::ble::WifiFailed, &auth_error, 2'000)) return;
  sleep_ms(150);
}

void script_session_reconnect(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  if(!phone.send_snapshot(snapshot, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(),
                                 snapshot.route_token,
                                 snapshot.route_generation, 3)) {
    return;
  }
  if(!phone.send_snapshot(snapshot, false)) return;

  phone.drop_link();
  sleep_ms(350);

  if(!phone.restart(0x51A8U)) return;
  moto::ble::NavigationSnapshot recovered = make_snapshot();
  recovered.route_generation = 5;
  recovered.road_name = "重连后道路";
  if(!phone.send_snapshot(recovered, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(),
                                 recovered.route_token,
                                 recovered.route_generation, 3)) {
    return;
  }
  if(!phone.send_snapshot(recovered, false)) return;
  sleep_ms(150);
}

void script_layout_rules(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  moto::ble::AckStatus status = moto::ble::AckStatus::Failed;

  if(!phone.send_layout_variant(3, 232, 4, &status)) return;
  if(status != moto::ble::AckStatus::Ok) {
    phone.fail("layout revision 3 was rejected");
    return;
  }
  if(!phone.await_layout_state(3, snapshot, 4'000)) return;

  if(!phone.send_layout_variant(2, 232, 2, &status)) return;
  if(status != moto::ble::AckStatus::Duplicate) {
    phone.fail("older layout revision was not reported as Duplicate");
    return;
  }
  if(!phone.send_layout_variant(3, 232, 2, &status)) return;
  if(status != moto::ble::AckStatus::Duplicate) {
    phone.fail("replayed layout revision was not reported as Duplicate");
    return;
  }

  // Encode a valid layout and corrupt map_height to 100: the decoder must
  // reject it and the bridge must not apply it.
  const moto::ble::UiLayout invalid = make_test_layout(4, 232, 2);
  const moto::ble::BytesResult encoded =
      moto::ble::encode_message(moto::ble::Message{invalid});
  if(!encoded.ok()) {
    phone.fail("could not encode the invalid-layout fixture");
    return;
  }
  moto::ble::Bytes payload = encoded.value;
  if(payload.size() < 6) {
    phone.fail("layout payload is unexpectedly short");
    return;
  }
  payload[4] = 100;
  payload[5] = 0;
  if(!phone.send_raw(moto::ble::MessageType::UiLayout,
                     moto::ble::ByteView(payload),
                     moto::ble::AckRequested)) {
    return;
  }
  if(!phone.wait_ack_status(phone.last_sequence(), &status, 2'000)) {
    phone.fail("no ACK for the invalid layout");
    return;
  }
  if(status != moto::ble::AckStatus::Failed) {
    phone.fail("invalid layout was not rejected with Failed");
    return;
  }

  if(!phone.send_layout_variant(4, 260, 2, &status)) return;
  if(status != moto::ble::AckStatus::Ok) {
    phone.fail("valid layout revision 4 was rejected");
    return;
  }
  if(!phone.await_layout_state(4, snapshot, 4'000)) return;
  sleep_ms(100);
}

void script_geometry_mismatch(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  if(!phone.send_snapshot(snapshot, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(),
                                 snapshot.route_token,
                                 snapshot.route_generation, 3)) {
    return;
  }
  if(!phone.send_snapshot(snapshot, false)) return;

  moto::ble::NavigationSnapshot next = make_snapshot();
  next.route_generation = 4;
  if(!phone.send_snapshot(next, true)) return;
  sleep_ms(150);
}

void script_geometry_recovery(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  if(!phone.send_snapshot(snapshot, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(),
                                 snapshot.route_token,
                                 snapshot.route_generation, 3)) {
    return;
  }
  if(!phone.send_snapshot(snapshot, false)) return;

  moto::ble::NavigationSnapshot next = make_snapshot();
  next.route_generation = 4;
  if(!phone.send_snapshot(next, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(), next.route_token, 4,
                                 3)) {
    return;
  }
  if(!phone.send_snapshot(next, false)) return;
  sleep_ms(150);
}

void script_wifi_errors(NavPhoneClient& phone) {
  if(!phone.begin()) return;

  moto::ble::WifiScanRequest request;
  request.request_id = 42;
  if(!phone.send_message(moto::ble::Message{request}, 0)) return;
  moto::ble::Message received;
  if(!phone.expect(moto::ble::MessageType::WifiScanResult, &received, 2'000)) {
    phone.fail("no WifiScanResult");
    return;
  }
  const auto* scan = std::get_if<moto::ble::WifiScanResult>(&received);
  if(scan == nullptr || scan->request_id != 42 ||
     scan->access_points.size() != 3) {
    phone.fail("WifiScanResult did not echo request 42 with three APs");
    return;
  }

  if(!phone.send_wifi_config("SIM-FAIL", moto::ble::WifiSaveAndConnect,
                             true)) {
    return;
  }
  std::uint8_t auth_error = moto::ble::WifiErrorAuth;
  if(!phone.wait_wifi_state(moto::ble::WifiFailed, &auth_error, 2'000)) return;

  if(!phone.send_wifi_config("SIM-TIMEOUT", moto::ble::WifiSaveAndConnect,
                             true)) {
    return;
  }
  std::uint8_t timeout_error = moto::ble::WifiErrorTimeout;
  if(!phone.wait_wifi_state(moto::ble::WifiFailed, &timeout_error, 2'000)) {
    return;
  }

  if(!phone.send_wifi_config("SIM-NET", moto::ble::WifiSaveAndConnect, true)) {
    return;
  }
  if(!phone.wait_wifi_state(moto::ble::WifiConnected, nullptr, 2'000)) return;

  if(!phone.send_wifi_config("", moto::ble::WifiDisconnect, true)) return;
  if(!phone.wait_wifi_state(moto::ble::WifiOff, nullptr, 2'000)) return;

  if(!phone.send_wifi_config("", moto::ble::WifiForget, true)) return;
  if(!phone.wait_wifi_state(moto::ble::WifiNoCredentials, nullptr, 2'000)) {
    return;
  }
  sleep_ms(100);
}

void script_state_acquiring(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  moto::ble::NavigationSnapshot snapshot = make_snapshot();
  snapshot.state = moto::ble::NavigationState::Acquiring;
  snapshot.maneuver = moto::ble::Maneuver::Unknown;
  snapshot.flags = 0;
  snapshot.road_name.clear();
  snapshot.instruction.clear();
  if(!phone.send_snapshot(snapshot, true)) return;
  sleep_ms(150);
}

void script_state_arrived(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  moto::ble::NavigationSnapshot snapshot = make_snapshot();
  snapshot.state = moto::ble::NavigationState::Arrived;
  snapshot.maneuver = moto::ble::Maneuver::Arrive;
  snapshot.flags = moto::ble::NavigationHasDestination |
                   moto::ble::NavigationHasFix;
  snapshot.distance_to_maneuver_m = 0;
  if(!phone.send_snapshot(snapshot, true)) return;
  sleep_ms(150);
}

void script_state_offline(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  moto::ble::NavigationSnapshot snapshot = make_snapshot();
  snapshot.network = moto::ble::NetworkState::Offline;
  if(!phone.send_snapshot(snapshot, true)) return;
  sleep_ms(150);
}

void script_state_rerouting(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  moto::ble::NavigationSnapshot snapshot = make_snapshot();
  snapshot.state = moto::ble::NavigationState::Rerouting;
  if(!phone.send_snapshot(snapshot, true)) return;
  sleep_ms(150);
}

void script_map_scene_stale(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  if(!phone.send_snapshot(snapshot, true)) return;
  if(!phone.send_map_scene(7, 2, 4, 1, 5)) return;

  moto::ble::AckStatus status = moto::ble::AckStatus::Failed;
  if(!phone.send_map_scene(6, 2, 4, 1, 5, &status)) return;
  if(status != moto::ble::AckStatus::Duplicate) {
    phone.fail("older map scene revision was not reported as Duplicate");
    return;
  }
  if(!phone.send_map_scene(8, 2, 4, 1, 5)) return;
  if(!phone.send_snapshot(snapshot, false)) return;
  sleep_ms(150);
}

void script_watchdog_offline(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  if(!phone.send_snapshot(snapshot, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(),
                                 snapshot.route_token,
                                 snapshot.route_generation, 3)) {
    return;
  }
  if(!phone.send_snapshot(snapshot, false)) return;

  // Stop heartbeats and all other frames: the device watchdog must drop the
  // logical link after its 3 s timeout while the TCP connection stays open.
  phone.set_heartbeat_enabled(false);
  sleep_ms(4'000);
}

void script_watchdog_recovery(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  const moto::ble::NavigationSnapshot snapshot = make_snapshot();
  if(!phone.send_snapshot(snapshot, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(),
                                 snapshot.route_token,
                                 snapshot.route_generation, 3)) {
    return;
  }
  if(!phone.send_snapshot(snapshot, false)) return;

  phone.set_heartbeat_enabled(false);
  sleep_ms(4'000);
  phone.set_heartbeat_enabled(true);

  // The first valid frame re-arms the link; geometry must be resent because
  // link loss clears the device-side route view.
  if(!phone.send_snapshot(snapshot, true)) return;
  if(!phone.send_geometry_chunks(visible_route_points(),
                                 snapshot.route_token,
                                 snapshot.route_generation, 3)) {
    return;
  }
  if(!phone.send_snapshot(snapshot, false)) return;
  sleep_ms(150);
}

void script_fragmentation(NavPhoneClient& phone) {
  if(!phone.begin()) return;
  if(!phone.send_max_map_scene(7)) return;

  moto::ble::AckStatus status = moto::ble::AckStatus::Failed;
  if(!phone.send_layout_variant(3, 232, 8, &status)) return;
  if(status != moto::ble::AckStatus::Ok) {
    phone.fail("eight-component layout was rejected");
    return;
  }
  if(!phone.await_layout_state(3, make_snapshot(), 4'000)) return;
  sleep_ms(150);
}

void run_script(NavPhoneClient& phone, CaseKind kind) {
  switch(kind) {
    case CaseKind::Baseline: script_baseline(phone); return;
    case CaseKind::SessionReconnect: script_session_reconnect(phone); return;
    case CaseKind::LayoutRules: script_layout_rules(phone); return;
    case CaseKind::GeometryMismatch: script_geometry_mismatch(phone); return;
    case CaseKind::GeometryRecovery: script_geometry_recovery(phone); return;
    case CaseKind::WifiErrors: script_wifi_errors(phone); return;
    case CaseKind::StateAcquiring: script_state_acquiring(phone); return;
    case CaseKind::StateArrived: script_state_arrived(phone); return;
    case CaseKind::StateOffline: script_state_offline(phone); return;
    case CaseKind::StateRerouting: script_state_rerouting(phone); return;
    case CaseKind::MapSceneStale: script_map_scene_stale(phone); return;
    case CaseKind::WatchdogOffline: script_watchdog_offline(phone); return;
    case CaseKind::WatchdogRecovery: script_watchdog_recovery(phone); return;
    case CaseKind::Fragmentation: script_fragmentation(phone); return;
  }
}

bool require(bool condition, const char* message, std::string* error) {
  if(condition) {
    return true;
  }
  if(error != nullptr) {
    *error = message;
  }
  return false;
}

bool verify_baseline(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.mode == MOTO_UI_NAVIGATING,
              "UI is not in the navigating mode", error)) {
    return false;
  }
  if(!require(state.maneuver == MOTO_MANEUVER_RIGHT,
              "UI did not map the right-turn maneuver", error)) {
    return false;
  }
  if(!require(state.distance_to_maneuver_m == 120,
              "UI did not receive the maneuver distance", error)) {
    return false;
  }
  if(!require(state.road_name != nullptr &&
                  std::string(state.road_name) == "泉城广场北路",
              "UI did not receive the road name", error)) {
    return false;
  }
  if(!require(state.route_point_count > 0,
              "UI did not receive the route view geometry", error)) {
    return false;
  }
  if(!require(state.page == MOTO_UI_PAGE_NAVIGATION,
              "UI page is not navigation", error)) {
    return false;
  }
  if(!require(state.online == 1, "UI did not mark the phone online", error)) {
    return false;
  }
  const moto_ui_layout_t& layout = sim.applied_layout();
  if(!require(layout.component_count == 4,
              "UI did not apply the four-component layout", error)) {
    return false;
  }
  if(!require(layout.map_height_px == 232,
              "UI did not apply the pushed map height", error)) {
    return false;
  }
  return true;
}

bool verify_session_reconnect(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.online == 1, "UI did not come back online", error)) {
    return false;
  }
  if(!require(state.route_generation == 5,
              "UI did not adopt the new route generation", error)) {
    return false;
  }
  if(!require(state.route_point_count > 0,
              "UI did not restore the route view after reconnect", error)) {
    return false;
  }
  if(!require(state.road_name != nullptr &&
                  std::string(state.road_name) == "重连后道路",
              "UI did not show the post-reconnect road", error)) {
    return false;
  }
  return true;
}

bool verify_layout_rules(DeviceSim& sim, std::string* error) {
  const moto_ui_layout_t& layout = sim.applied_layout();
  if(!require(layout.component_count == 2,
              "UI did not apply the revision-4 component count", error)) {
    return false;
  }
  if(!require(layout.map_height_px == 260,
              "UI did not apply the revision-4 map height", error)) {
    return false;
  }
  return true;
}

bool verify_geometry_mismatch(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.route_generation == 4,
              "UI did not adopt the newer route generation", error)) {
    return false;
  }
  if(!require(state.route_point_count == 0,
              "UI kept a stale route view after a generation mismatch",
              error)) {
    return false;
  }
  if(!require(state.mode == MOTO_UI_NAVIGATING,
              "UI left the navigating mode", error)) {
    return false;
  }
  return true;
}

bool verify_geometry_recovery(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.route_generation == 4,
              "UI did not adopt the recovered route generation", error)) {
    return false;
  }
  if(!require(state.route_point_count > 0,
              "UI did not restore the route view for generation 4", error)) {
    return false;
  }
  return true;
}

bool verify_wifi_errors(DeviceSim&, std::string*) {
  // Every WiFi transition (auth failure, timeout, success, disconnect, forget)
  // is asserted inside the scripted protocol exchange.
  return true;
}

bool verify_state_acquiring(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  return require(state.mode == MOTO_UI_ACQUIRING_FIX,
                 "UI is not in the acquiring-fix state", error);
}

bool verify_state_arrived(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.mode == MOTO_UI_ARRIVED,
              "UI is not in the arrived state", error)) {
    return false;
  }
  if(!require(state.maneuver == MOTO_MANEUVER_ARRIVE,
              "UI did not map the arrive maneuver", error)) {
    return false;
  }
  return true;
}

bool verify_state_offline(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.mode == MOTO_UI_OFFLINE,
              "UI is not in the offline navigation state", error)) {
    return false;
  }
  if(!require(state.online == 0, "UI still marks the phone online", error)) {
    return false;
  }
  return true;
}

bool verify_state_rerouting(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  return require(state.mode == MOTO_UI_REROUTING,
                 "UI is not in the rerouting state", error);
}

bool verify_map_scene_stale(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  return require(state.map_scene_revision == 8,
                 "UI did not adopt the newest map scene revision", error);
}

bool verify_watchdog_offline(DeviceSim& sim, std::string* error) {
  if(!require(!sim.transport().link_active(),
              "link watchdog did not drop the logical link", error)) {
    return false;
  }
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.mode == MOTO_UI_OFFLINE,
              "UI did not switch to offline after link loss", error)) {
    return false;
  }
  if(!require(state.route_point_count == 0,
              "route view survived link loss", error)) {
    return false;
  }
  return true;
}

bool verify_watchdog_recovery(DeviceSim& sim, std::string* error) {
  if(!require(sim.transport().link_active(),
              "link did not recover after a valid frame", error)) {
    return false;
  }
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.online == 1, "UI did not return online", error)) {
    return false;
  }
  if(!require(state.route_point_count > 0,
              "route view was not restored after watchdog recovery", error)) {
    return false;
  }
  return true;
}

bool verify_fragmentation(DeviceSim& sim, std::string* error) {
  const moto_ui_state_t& state = sim.presenter().ui_state();
  if(!require(state.map_scene_revision == 7,
              "UI did not adopt the maximum-size map scene", error)) {
    return false;
  }
  const moto_ui_layout_t& layout = sim.applied_layout();
  if(!require(layout.component_count == 8,
              "UI did not apply the eight-component layout", error)) {
    return false;
  }
  return true;
}

struct CaseSpec {
  const char* id;
  const char* description;
  CaseKind kind;
  bool (*verify)(DeviceSim&, std::string*);
};

constexpr CaseSpec kCases[] = {
    {"baseline",
     "handshake, navigation, geometry, map scene, media, layout and WiFi",
     CaseKind::Baseline, &verify_baseline},
    {"session-reconnect",
     "link loss, new session id and full geometry recovery",
     CaseKind::SessionReconnect, &verify_session_reconnect},
    {"layout-rules",
     "monotonic revisions, replay, invalid payload rejection and debounce",
     CaseKind::LayoutRules, &verify_layout_rules},
    {"geometry-mismatch",
     "a newer snapshot generation hides stale route geometry",
     CaseKind::GeometryMismatch, &verify_geometry_mismatch},
    {"geometry-recovery",
     "geometry for the new generation restores the route view",
     CaseKind::GeometryRecovery, &verify_geometry_recovery},
    {"wifi-errors",
     "scan, auth failure, timeout, success, disconnect and forget",
     CaseKind::WifiErrors, &verify_wifi_errors},
    {"state-acquiring",
     "no destination renders the acquiring-fix state",
     CaseKind::StateAcquiring, &verify_state_acquiring},
    {"state-arrived",
     "arrival renders the arrived state and maneuver",
     CaseKind::StateArrived, &verify_state_arrived},
    {"state-offline",
     "offline network keeps navigation but marks the UI offline",
     CaseKind::StateOffline, &verify_state_offline},
    {"state-rerouting",
     "rerouting renders the rerouting state",
     CaseKind::StateRerouting, &verify_state_rerouting},
    {"map-scene-stale",
     "older map scene revisions are rejected and the newest is adopted",
     CaseKind::MapSceneStale, &verify_map_scene_stale},
    {"watchdog-offline",
     "missing phone frames drop the logical link and clear the route",
     CaseKind::WatchdogOffline, &verify_watchdog_offline},
    {"watchdog-recovery",
     "a valid frame after watchdog expiry restores the link and route",
     CaseKind::WatchdogRecovery, &verify_watchdog_recovery},
    {"fragmentation",
     "maximum MapScene and eight-component layout exercise multi-frame paths",
     CaseKind::Fragmentation, &verify_fragmentation},
};

const CaseSpec* find_case(const std::string& id) {
  for(const auto& spec : kCases) {
    if(id == spec.id) {
      return &spec;
    }
  }
  return nullptr;
}

}  // namespace

std::vector<SimCaseInfo> cases() {
  std::vector<SimCaseInfo> result;
  result.reserve(sizeof(kCases) / sizeof(kCases[0]));
  for(const auto& spec : kCases) {
    result.push_back(SimCaseInfo{spec.id, spec.description});
  }
  return result;
}

bool run_case(DeviceSim& sim, const std::string& id, bool dump_frames,
              std::string* error) {
  const CaseSpec* spec = find_case(id);
  if(spec == nullptr) {
    if(error != nullptr) {
      *error = "unknown case: " + id;
    }
    return false;
  }

  const std::uint64_t initial_hash = sim.display().frame_hash();
  if(dump_frames) {
    std::string dump_error;
    if(!sim.dump("01-offline-" + id, &dump_error)) {
      if(error != nullptr) *error = dump_error;
      return false;
    }
  }

  std::atomic<bool> finished{false};
  std::atomic<bool> release{false};
  std::string script_error;
  std::thread worker([&] {
    NavPhoneClient phone(sim.port());
    run_script(phone, spec->kind);
    script_error = phone.error();
    finished.store(true);
    // Keep the phone session alive until the device state has been verified.
    while(!release.load(std::memory_order_acquire)) {
      sleep_ms(2);
    }
  });

  const std::uint64_t deadline = now_ms() + 25'000;
  while(!finished.load() && now_ms() < deadline) {
    sim.tick(now_ms());
    sleep_ms(5);
  }
  const bool timed_out = !finished.load();

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
    failure = "case timed out";
  } else if(!script_error.empty()) {
    ok = false;
    failure = script_error;
  } else if(!sim.transport().protocol_ready()) {
    ok = false;
    failure = "transport never reached protocol Ready";
  } else if(!spec->verify(sim, &failure)) {
    ok = false;
  } else if(initial_hash == sim.display().frame_hash()) {
    ok = false;
    failure = "the framebuffer did not change after the phone session";
  } else if(dump_frames && !sim.dump("02-" + id, &failure)) {
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

bool run_selftest(DeviceSim& sim, bool dump_frames, std::string* error) {
  return run_case(sim, "baseline", dump_frames, error);
}

}  // namespace moto::sim
