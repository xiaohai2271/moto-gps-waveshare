#include "phone_nav_bridge.h"
#include "phone_nav_bridge_test_probe.hpp"
#include "demo_fixture/jinan_big_data_demo.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void pump(PhoneNavBridge& bridge) {
  bridge.render_pending_for_test();
}

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__                             \
                << " CHECK failed: " #condition << '\n';                  \
      ++failures;                                                          \
    }                                                                      \
  } while (false)

moto::ble::ReassembledMessage make_message(
    const moto::ble::Message& message,
    std::uint16_t sequence) {
  const moto::ble::BytesResult encoded = moto::ble::encode_message(message);
  if (!encoded.ok()) {
    std::cerr << "Could not encode bridge test message\n";
    ++failures;
    return {};
  }
  moto::ble::ReassembledMessage result;
  result.type = moto::ble::message_type(message);
  result.sequence = sequence;
  result.payload = encoded.value;
  return result;
}

struct ReentrantSenderContext {
  PhoneNavBridge* bridge = nullptr;
  int calls = 0;
  moto::ble::DisplayPage last_page = moto::ble::DisplayPage::Navigation;
};

bool reentrant_sender(const moto::ble::Message& message,
                      std::uint8_t,
                      void* context) {
  auto* sender = static_cast<ReentrantSenderContext*>(context);
  const auto* command = std::get_if<moto::ble::DeviceCommand>(&message);
  if (command == nullptr) {
    return false;
  }
  ++sender->calls;
  sender->last_page = command->page;

  // A real transport can synchronously reject a send or cause a link callback.
  // This re-entry deadlocks if PhoneNavBridge keeps state_mutex_ while sending.
  sender->bridge->on_link_state(false);
  return true;
}

struct MusicSenderContext {
  std::vector<moto::ble::DeviceCommand> commands;
  std::vector<std::uint8_t> flags;
};

bool record_music_sender(const moto::ble::Message& message,
                         std::uint8_t flags,
                         void* context) {
  auto* sender = static_cast<MusicSenderContext*>(context);
  const auto* command = std::get_if<moto::ble::DeviceCommand>(&message);
  if (command == nullptr) return false;
  sender->commands.push_back(*command);
  sender->flags.push_back(flags);
  return true;
}

void test_sender_is_called_without_bridge_state_lock() {
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  ReentrantSenderContext sender{&bridge};
  bridge.set_sender(reentrant_sender, &sender);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);

  moto::test::emit_page_change(MOTO_UI_PAGE_SPEED);
  CHECK(sender.calls == 1);
  CHECK(sender.last_page == moto::ble::DisplayPage::Speed);
}

void test_navigation_and_touch_updates_are_serialized() {
  moto::test::reset_phone_nav_bridge_probe();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);

  constexpr int kIterations = 2'000;
  std::atomic<bool> begin{false};
  std::thread phone([&]() {
    while (!begin.load()) {
      std::this_thread::yield();
    }
    for (int index = 0; index < kIterations; ++index) {
      moto::ble::NavigationSnapshot snapshot;
      snapshot.state = moto::ble::NavigationState::Navigating;
      snapshot.network = moto::ble::NetworkState::Online;
      snapshot.flags = moto::ble::NavigationHasNextManeuver;
      snapshot.maneuver = (index % 2 == 0)
                              ? moto::ble::Maneuver::Left
                              : moto::ble::Maneuver::Right;
      snapshot.route_token = 17;
      snapshot.route_generation = 3;
      snapshot.road_name = "并发道路-" + std::to_string(index) +
                           std::string(32, static_cast<char>('a' + index % 26));
      snapshot.distance_to_maneuver_m = static_cast<std::uint32_t>(index);
      const auto message = make_message(
          moto::ble::Message{snapshot},
          static_cast<std::uint16_t>(index % 65'535 + 1));
      CHECK(bridge.on_message(message) == moto::ble::AckStatus::Ok);
    }
  });

  std::thread touch([&]() {
    while (!begin.load()) {
      std::this_thread::yield();
    }
    for (int index = 0; index < kIterations; ++index) {
      moto::test::emit_page_change(
          index % 2 == 0 ? MOTO_UI_PAGE_SPEED : MOTO_UI_PAGE_COMPASS);
    }
  });

  begin.store(true);
  phone.join();
  touch.join();

  // Thousands of producer updates collapse into the latest complete state;
  // none of the producer threads enters LVGL.
  CHECK(moto::test::phone_nav_bridge_apply_count() == 0);
  CHECK(moto::test::phone_nav_bridge_board_lock_count() == 0);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_apply_count() == 1);
  const std::string final_name = moto::test::phone_nav_bridge_last_road_name();
  CHECK(final_name.rfind("并发道路-", 0) == 0);
}

void test_ble_submission_never_waits_for_lvgl_and_retries_latest_state() {
  moto::test::reset_phone_nav_bridge_probe();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);

  moto::ble::NavigationSnapshot first;
  first.state = moto::ble::NavigationState::Navigating;
  first.network = moto::ble::NetworkState::Online;
  first.flags = moto::ble::NavigationHasNextManeuver;
  first.route_token = 29;
  first.route_generation = 1;
  first.maneuver = moto::ble::Maneuver::Left;
  first.road_name = "旧状态";

  moto::test::phone_nav_bridge_set_board_lock_available(false);
  CHECK(bridge.on_message(make_message(first, 1)) ==
        moto::ble::AckStatus::Ok);
  CHECK(moto::test::phone_nav_bridge_board_lock_count() == 0);
  CHECK(moto::test::phone_nav_bridge_apply_count() == 0);

  // Only the render consumer attempts LVGL. Failure keeps the dirty bit and
  // the producer remains completely decoupled from that unavailable lock.
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_board_lock_count() == 1);
  CHECK(moto::test::phone_nav_bridge_apply_count() == 0);

  auto latest = first;
  latest.maneuver = moto::ble::Maneuver::Right;
  latest.road_name = "保留的最新状态";
  CHECK(bridge.on_message(make_message(latest, 2)) ==
        moto::ble::AckStatus::Ok);
  CHECK(moto::test::phone_nav_bridge_board_lock_count() == 1);

  moto::test::phone_nav_bridge_set_board_lock_available(true);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_apply_count() == 1);
  CHECK(moto::test::phone_nav_bridge_last_road_name() == "保留的最新状态");
  CHECK(moto::test::phone_nav_bridge_last_maneuver() ==
        MOTO_MANEUVER_RIGHT);
}

void test_phone_connection_lifecycle_reaches_the_ui() {
  moto::test::reset_phone_nav_bridge_probe();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);

  bridge.on_link_state(true);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_phone_connection() ==
        MOTO_UI_PHONE_CONNECTING);

  moto::ble::ConnectionStatus starting;
  starting.role = moto::ble::EndpointRole::Phone;
  starting.state = moto::ble::ConnectionState::Starting;
  starting.minimum_version = moto::ble::kProtocolVersion;
  starting.maximum_version = moto::ble::kProtocolVersion;
  starting.session_id = 1234;
  CHECK(bridge.on_message(make_message(starting, 10)) ==
        moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_phone_connection() ==
        MOTO_UI_PHONE_CONNECTING);

  starting.state = moto::ble::ConnectionState::Ready;
  CHECK(bridge.on_message(make_message(starting, 11)) ==
        moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_phone_connection() ==
        MOTO_UI_PHONE_ONLINE);

  bridge.on_link_state(false);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_phone_connection() ==
        MOTO_UI_PHONE_OFFLINE);
}

void test_music_capability_state_and_touch_commands() {
  moto::test::reset_phone_nav_bridge_probe();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  MusicSenderContext sender;
  bridge.set_sender(record_music_sender, &sender);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);

  moto::ble::ConnectionStatus ready;
  ready.role = moto::ble::EndpointRole::Phone;
  ready.state = moto::ble::ConnectionState::Starting;
  ready.minimum_version = moto::ble::kProtocolVersion;
  ready.maximum_version = moto::ble::kProtocolVersion;
  ready.capabilities = moto::ble::CapabilityMediaState |
                       moto::ble::CapabilityMusicCommands |
                       moto::ble::CapabilityCommandAck;
  ready.session_id = 4321;
  CHECK(bridge.on_message(make_message(ready, 20)) ==
        moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(!moto::test::phone_nav_bridge_music_page_enabled());
  ready.state = moto::ble::ConnectionState::Ready;
  CHECK(bridge.on_message(make_message(ready, 21)) ==
        moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_music_page_enabled());

  moto::ble::MediaState media;
  media.flags = moto::ble::MediaConnected |
                moto::ble::MediaPlaying |
                moto::ble::MediaLikeAvailable;
  media.track_token = 88;
  media.position_s = 12;
  media.duration_s = 240;
  media.source_name = "APPLE MUSIC";
  media.track_title = "夜间骑行";
  media.artist_name = "MOTO GPS";
  CHECK(bridge.on_message(make_message(media, 22)) ==
        moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_media_connected());
  CHECK(moto::test::phone_nav_bridge_media_playing());
  CHECK(moto::test::phone_nav_bridge_media_like_available());
  CHECK(moto::test::phone_nav_bridge_media_source() == "APPLE MUSIC");
  CHECK(moto::test::phone_nav_bridge_media_title() == "夜间骑行");
  CHECK(moto::test::phone_nav_bridge_media_artist() == "MOTO GPS");

  moto::test::emit_music_command(MOTO_MUSIC_PREVIOUS);
  moto::test::emit_music_command(MOTO_MUSIC_TOGGLE_PLAYBACK);
  moto::test::emit_music_command(MOTO_MUSIC_NEXT);
  moto::test::emit_music_command(MOTO_MUSIC_LIKE);
  CHECK(sender.commands.size() == 4);
  const moto::ble::DeviceCommandKind expected[] = {
      moto::ble::DeviceCommandKind::MusicPrevious,
      moto::ble::DeviceCommandKind::MusicTogglePlayback,
      moto::ble::DeviceCommandKind::MusicNext,
      moto::ble::DeviceCommandKind::MusicLike,
  };
  for (std::size_t index = 0; index < sender.commands.size(); ++index) {
    CHECK(sender.commands[index].kind == expected[index]);
    CHECK(sender.commands[index].page == moto::ble::DisplayPage::Music);
    CHECK(sender.commands[index].command_id == index + 1);
    CHECK((sender.flags[index] & moto::ble::AckRequested) != 0U);
    CHECK((sender.flags[index] & moto::ble::Urgent) != 0U);
  }
}

void test_demo_uses_the_production_presenter_path() {
  moto::test::reset_phone_nav_bridge_probe();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  bridge.install_ui_callbacks();

  moto::test::emit_demo_change(true);
  pump(bridge);
  const auto now_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());

  // Approach and cross the first junction in the real OSM route. The symbol
  // and distance switch together: right into 新泺大街, then left into 崇华路.
  // Distances come from cumulative route geometry, not UI timer thresholds.
  bridge.update_demo(now_ms + 6'200);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_maneuver() ==
        MOTO_MANEUVER_RIGHT);
  CHECK(moto::test::phone_nav_bridge_last_distance_to_maneuver_m() > 3);
  CHECK(moto::test::phone_nav_bridge_last_distance_to_maneuver_m() < 8);
  bridge.update_demo(now_ms + 7'000);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_maneuver() ==
        MOTO_MANEUVER_LEFT);
  CHECK(moto::test::phone_nav_bridge_last_distance_to_maneuver_m() > 135);
  CHECK(moto::test::phone_nav_bridge_last_distance_to_maneuver_m() < 141);

  bridge.update_demo(now_ms + 12'000);
  pump(bridge);

  CHECK(moto::test::phone_nav_bridge_last_demo_active());
  CHECK(moto::test::phone_nav_bridge_last_route_point_count() >= 2);
  // The 232 px default map viewport clips context behind the rider; the demo
  // fixture itself may contain more points than fit on screen.
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() > 0);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() <=
        moto::demo::jinan_big_data::kRoadPoints.size());
  CHECK(moto::test::phone_nav_bridge_last_road_polyline_count() > 0);
  CHECK(moto::test::phone_nav_bridge_last_road_polyline_count() <=
        moto::demo::jinan_big_data::kRoads.size());
  CHECK(moto::test::phone_nav_bridge_apply_count() >= 2);

  bridge.update_demo(now_ms + 6'200);
  pump(bridge);
  const uint16_t heading_before =
      moto::test::phone_nav_bridge_last_heading_deg();
  bridge.update_demo(now_ms + 8'500);
  pump(bridge);
  const uint16_t heading_after =
      moto::test::phone_nav_bridge_last_heading_deg();
  CHECK(heading_before != heading_after);

  moto::test::emit_demo_change(false);
  pump(bridge);
  CHECK(!moto::test::phone_nav_bridge_last_demo_active());
}

void test_ios_demo_token_attaches_context_and_live_route_clears_it() {
  moto::test::reset_phone_nav_bridge_probe();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);

  const std::uint32_t demo_token =
      moto::ble::route_token(moto::demo::jinan_big_data::kRouteId);
  moto::ble::NavigationSnapshot snapshot;
  snapshot.state = moto::ble::NavigationState::Navigating;
  snapshot.network = moto::ble::NetworkState::Online;
  snapshot.flags = moto::ble::NavigationHasRouteView;
  snapshot.route_token = demo_token;
  snapshot.route_generation = 1;
  moto::ble::RouteGeometry geometry;
  geometry.route_token = demo_token;
  geometry.route_generation = 1;
  geometry.total_point_count = 2;
  geometry.view_origin = {36'674'773, 117'128'056};
  geometry.points = {
      {36'674'773, 117'128'056},
      {36'674'145, 117'128'489},
  };
  // The phone queues changed geometry before the snapshot that references it.
  // Geometry updates state only; the following snapshot is the single LVGL
  // presentation boundary for the pair.
  const int before_pair = moto::test::phone_nav_bridge_apply_count();
  CHECK(bridge.on_message(make_message(geometry, 40)) ==
        moto::ble::AckStatus::Ok);
  CHECK(moto::test::phone_nav_bridge_apply_count() == before_pair);
  CHECK(bridge.on_message(make_message(snapshot, 41)) ==
        moto::ble::AckStatus::Ok);
  CHECK(moto::test::phone_nav_bridge_apply_count() == before_pair);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_apply_count() == before_pair + 1);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() > 0);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() <=
        moto::demo::jinan_big_data::kRoadPoints.size());
  CHECK(moto::test::phone_nav_bridge_last_road_polyline_count() > 0);
  CHECK(moto::test::phone_nav_bridge_last_road_polyline_count() <=
        moto::demo::jinan_big_data::kRoads.size());

  snapshot.flags = 0;
  snapshot.route_token = moto::ble::route_token("amap-live-route");
  snapshot.route_generation = 2;
  CHECK(bridge.on_message(make_message(snapshot, 42)) ==
        moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() == 0);
  CHECK(moto::test::phone_nav_bridge_last_road_polyline_count() == 0);
}

void test_map_scene_atomically_replaces_roads_and_buildings_at_capacity() {
  moto::test::reset_phone_nav_bridge_probe();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);

  moto::ble::NavigationSnapshot navigation;
  navigation.state = moto::ble::NavigationState::Navigating;
  navigation.network = moto::ble::NetworkState::Online;
  navigation.flags = moto::ble::NavigationHasRouteView;
  navigation.route_token = 77;
  navigation.route_generation = 1;
  navigation.heading_cdeg = 9'000;
  CHECK(bridge.on_message(make_message(navigation, 50)) ==
        moto::ble::AckStatus::Ok);

  moto::ble::RouteGeometry geometry;
  geometry.route_token = 77;
  geometry.route_generation = 1;
  geometry.total_point_count = 2;
  geometry.view_origin = {36'670'000, 117'130'000};
  geometry.points = {
      geometry.view_origin,
      {36'670'000, 117'131'000},
  };
  CHECK(bridge.on_message(make_message(geometry, 51)) ==
        moto::ble::AckStatus::Ok);

  moto::ble::MapScene first;
  first.scene_revision = 1;
  first.view_origin = geometry.view_origin;
  first.radius_m = 500;
  first.roads = {
      {moto::ble::MapRoadClass::Primary,
       {{36'670'000, 117'130'000}, {36'670'000, 117'131'000}}},
  };
  first.buildings = {
      {moto::ble::MapBuildingClass::Landmark,
       {{36'670'100, 117'130'100},
        {36'670'100, 117'130'200},
        {36'670'200, 117'130'200}}},
  };
  const int before_first = moto::test::phone_nav_bridge_apply_count();
  CHECK(bridge.on_message(make_message(first, 52)) ==
        moto::ble::AckStatus::Ok);
  CHECK(moto::test::phone_nav_bridge_apply_count() == before_first);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_apply_count() == before_first + 1);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() == 2);
  CHECK(moto::test::phone_nav_bridge_last_road_polyline_count() == 1);
  CHECK(moto::test::phone_nav_bridge_last_road_class() ==
        static_cast<std::uint8_t>(moto::ble::MapRoadClass::Primary));
  CHECK(moto::test::phone_nav_bridge_last_building_point_count() == 3);
  CHECK(moto::test::phone_nav_bridge_last_building_footprint_count() == 1);
  CHECK(moto::test::phone_nav_bridge_last_building_class() ==
        static_cast<std::uint8_t>(moto::ble::MapBuildingClass::Landmark));

  moto::ble::MapScene maximum;
  maximum.scene_revision = 2;
  maximum.view_origin = geometry.view_origin;
  maximum.radius_m = 700;
  for (std::size_t road_index = 0;
       road_index < moto::ble::kMaxMapSceneRoads; ++road_index) {
    moto::ble::MapRoadPolyline road;
    road.road_class = road_index == 0
                          ? moto::ble::MapRoadClass::Service
                          : moto::ble::MapRoadClass::Residential;
    for (std::size_t point_index = 0; point_index < 8; ++point_index) {
      road.points.push_back({
          geometry.view_origin.latitude_e6 +
              static_cast<std::int32_t>(road_index * 20 + point_index),
          geometry.view_origin.longitude_e6 +
              static_cast<std::int32_t>(road_index * 25 + point_index * 3),
      });
    }
    maximum.roads.push_back(std::move(road));
  }
  for (std::size_t building_index = 0;
       building_index < moto::ble::kMaxMapSceneBuildings;
       ++building_index) {
    moto::ble::MapBuildingFootprint building;
    building.building_class = moto::ble::MapBuildingClass::Parking;
    for (std::size_t point_index = 0; point_index < 8; ++point_index) {
      building.points.push_back({
          geometry.view_origin.latitude_e6 +
              static_cast<std::int32_t>(building_index * 30 + point_index),
          geometry.view_origin.longitude_e6 +
              static_cast<std::int32_t>(building_index * 35 + point_index * 2),
      });
    }
    maximum.buildings.push_back(std::move(building));
  }

  const int before_maximum = moto::test::phone_nav_bridge_apply_count();
  CHECK(bridge.on_message(make_message(maximum, 53)) ==
        moto::ble::AckStatus::Ok);
  CHECK(moto::test::phone_nav_bridge_apply_count() == before_maximum);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_apply_count() == before_maximum + 1);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() ==
        moto::ble::kMaxMapSceneRoadPoints);
  CHECK(moto::test::phone_nav_bridge_last_road_polyline_count() ==
        moto::ble::kMaxMapSceneRoads);
  CHECK(moto::test::phone_nav_bridge_last_road_class() ==
        static_cast<std::uint8_t>(moto::ble::MapRoadClass::Service));
  CHECK(moto::test::phone_nav_bridge_last_building_point_count() ==
        moto::ble::kMaxMapSceneBuildingPoints);
  CHECK(moto::test::phone_nav_bridge_last_building_footprint_count() ==
        moto::ble::kMaxMapSceneBuildings);
  CHECK(moto::test::phone_nav_bridge_last_building_class() ==
        static_cast<std::uint8_t>(moto::ble::MapBuildingClass::Parking));

  const int before_stale = moto::test::phone_nav_bridge_apply_count();
  CHECK(bridge.on_message(make_message(first, 54)) ==
        moto::ble::AckStatus::Duplicate);
  CHECK(moto::test::phone_nav_bridge_apply_count() == before_stale);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() ==
        moto::ble::kMaxMapSceneRoadPoints);
  CHECK(moto::test::phone_nav_bridge_last_building_point_count() ==
        moto::ble::kMaxMapSceneBuildingPoints);

  moto::ble::MapScene empty;
  empty.scene_revision = 3;
  empty.view_origin = geometry.view_origin;
  empty.radius_m = 500;
  CHECK(bridge.on_message(make_message(empty, 55)) ==
        moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() == 0);
  CHECK(moto::test::phone_nav_bridge_last_road_polyline_count() == 0);
  CHECK(moto::test::phone_nav_bridge_last_building_point_count() == 0);
  CHECK(moto::test::phone_nav_bridge_last_building_footprint_count() == 0);
}

void test_imu_presentation_preserves_40hz_phase_and_stale_course_is_ignored() {
  moto::test::reset_phone_nav_bridge_probe();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  // The IMU is usually ready before BLE. Its relative zero cannot become the
  // absolute heading when the phone finally connects.
  bridge.on_imu_sample(0.0F, 1);
  moto::ble::NavigationSnapshot navigation;
  navigation.state = moto::ble::NavigationState::Navigating;
  navigation.display_page = moto::ble::DisplayPage::Compass;
  navigation.flags = moto::ble::NavigationHasFix;
  navigation.heading_cdeg = 9'000;
  navigation.speed_deci_kph = 360;
  CHECK(bridge.on_message(make_message(navigation, 1)) == moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_heading_deg() == 90);

  const int before = moto::test::phone_nav_bridge_apply_count();
  for (std::uint64_t time = 8; time <= 1'000; time += 8) {
    bridge.on_imu_sample(45.0F, time);
    pump(bridge);
  }
  const int motion_frames = moto::test::phone_nav_bridge_apply_count() - before;
  CHECK(motion_frames == 40);  // resetting the deadline yields only 31
  const auto before_stale = moto::test::phone_nav_bridge_last_heading_deg();
  navigation.heading_cdeg = 27'000;
  navigation.flags |= moto::ble::NavigationGnssStale;
  CHECK(bridge.on_message(make_message(navigation, 2)) == moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_heading_deg() == before_stale);
}

// --- Editable navigation layout (docs/NAV_LAYOUT_EDITOR_PLAN.md §5.3/§5.6/§6.5) ---

// Captures the layout bookkeeping messages the bridge emits so a test can assert
// what the phone is told about the layout in effect.
struct LayoutSenderContext {
  int messages = 0;
  int states = 0;
  moto::ble::UiLayoutState last{};
};

bool record_layout_sender(const moto::ble::Message& message,
                          std::uint8_t,
                          void* context) {
  auto* sender = static_cast<LayoutSenderContext*>(context);
  ++sender->messages;
  if (const auto* state = std::get_if<moto::ble::UiLayoutState>(&message)) {
    ++sender->states;
    sender->last = *state;
  }
  return true;
}

// A layout that exercises every field the bridge has to transcribe, with the
// eight components in a deliberately non-default shape.
moto::ble::UiLayout layout_fixture(std::uint16_t revision) {
  moto::ble::UiLayout layout;
  layout.layout_revision = revision;
  layout.flags = moto::ble::UiLayoutNorthUp |
                 moto::ble::UiLayoutMusicPageEnabled |
                 moto::ble::UiLayoutMapVisible;
  layout.theme = static_cast<std::uint8_t>(moto::ble::UiThemeId::Light);
  layout.map_height_px = 320;
  layout.map_opacity = 70;
  layout.route_r = 0x12;
  layout.route_g = 0x34;
  layout.route_b = 0x56;
  layout.route_width_px = 9;
  layout.route_opacity = 60;
  layout.street_r = 0x65;
  layout.street_g = 0x43;
  layout.street_b = 0x21;
  layout.street_width_px = 5;
  layout.street_opacity = 45;
  layout.building_opacity = 15;
  layout.layer_mask = moto::ble::UiLayerRoads | moto::ble::UiLayerRoute;
  layout.zoom_percent = 150;
  layout.components = {
      // id, flags(enabled), size, opacity, x, y, anchor, span_deg
      {1, 1, 0, 100, 10, 20, 0, 0},     // speed, S
      {2, 0, 2, 90, 300, 240, 0, 0},    // speed limit, L, hidden
      {3, 1, 1, 55, 24, 40, 0, 0},      // heading
      {4, 1, 1, 100, 30, 210, 0, 0},    // maneuver
      {5, 1, 0, 80, 120, 390, 0, 0},    // remaining distance
      {6, 0, 2, 70, 260, 390, 0, 0},    // remaining time
      {7, 1, 1, 65, 0, 0, 0, 180},      // progress arc, top, 180 deg
      {8, 1, 2, 35, 0, 0, 2, 90},       // speed arc, bottom, 90 deg
  };
  return layout;
}

// apply_pending_layout() measures the debounce with the monotonic clock, so a
// host test has to let the real 2 s window elapse (plan §5.6).
void wait_for_layout_debounce() {
  std::this_thread::sleep_for(std::chrono::milliseconds(2'100));
}

void test_layout_is_applied_only_after_the_debounce_and_is_persisted() {
  moto::test::reset_phone_nav_bridge_probe();
  moto_ui_layout_store::factory_reset();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  LayoutSenderContext sender;
  bridge.set_sender(record_layout_sender, &sender);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);

  // No record on flash yet: the constructor's restore falls back to the
  // documented default layout.
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() >= 1);
  const int baseline = moto::test::phone_nav_bridge_layout_apply_count();
  moto_ui_layout_t restored = moto::test::phone_nav_bridge_last_applied_layout();
  CHECK(restored.north_up == 1);
  CHECK(restored.map_height_px == MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT);
  CHECK(restored.zoom_percent == 100);
  CHECK(restored.component_count == MOTO_UI_LAYOUT_COMPONENT_COUNT);
  moto_ui_layout_t unused{};
  std::uint16_t unused_revision = 0;
  CHECK(!moto_ui_layout_store::load(&unused, &unused_revision));

  // A push is acknowledged immediately but not applied in the same frame: the
  // debounce window collapses a drag burst into a single flash write.
  const moto::ble::UiLayout pushed = layout_fixture(7);
  CHECK(bridge.on_message(make_message(moto::ble::Message{pushed}, 1)) ==
        moto::ble::AckStatus::Ok);
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() == baseline);
  bridge.apply_pending_layout();
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() == baseline);

  wait_for_layout_debounce();
  bridge.apply_pending_layout();
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() == baseline + 1);

  const moto_ui_layout_t applied =
      moto::test::phone_nav_bridge_last_applied_layout();
  CHECK(applied.north_up == 1);
  CHECK(applied.music_page_enabled == 1);
  CHECK(applied.map_visible == 1);
  CHECK(applied.theme_id == static_cast<std::uint8_t>(moto::ble::UiThemeId::Light));
  CHECK(applied.map_height_px == 320);
  CHECK(applied.map_opacity == 70);
  CHECK(applied.route_r == 0x12);
  CHECK(applied.route_g == 0x34);
  CHECK(applied.route_b == 0x56);
  CHECK(applied.route_width_px == 9);
  CHECK(applied.route_opacity == 60);
  CHECK(applied.street_width_px == 5);
  CHECK(applied.street_opacity == 45);
  CHECK(applied.building_opacity == 15);
  CHECK(applied.layer_mask == (moto::ble::UiLayerRoads | moto::ble::UiLayerRoute));
  CHECK(applied.zoom_percent == 150);
  CHECK(applied.component_count == 8);
  CHECK(applied.components[0].id == MOTO_UI_COMPONENT_SPEED);
  CHECK(applied.components[0].size == MOTO_UI_SIZE_S);
  CHECK(applied.components[1].enabled == 0);
  CHECK(applied.components[6].anchor == MOTO_UI_ARC_TOP);
  CHECK(applied.components[6].span_deg == 180);
  CHECK(applied.components[7].anchor == MOTO_UI_ARC_BOTTOM);
  CHECK(applied.components[7].span_deg == 90);

  // ...and the accepted layout is durable: a fresh boot reads it back at the
  // same revision instead of reverting to the default.
  moto_ui_layout_t stored{};
  std::uint16_t stored_revision = 0;
  CHECK(moto_ui_layout_store::load(&stored, &stored_revision));
  CHECK(stored_revision == 7);
  CHECK(std::memcmp(&stored, &applied, sizeof(stored)) == 0);

  // The phone is told the layout is in effect and safely stored (§5.4).
  CHECK(sender.states >= 1);
  CHECK(sender.last.layout_revision == 7);
  CHECK(sender.last.supported_mask == 0xFF);
  CHECK((sender.last.flags & moto::ble::UiLayoutNvsSaved) != 0U);
}

void test_layout_duplicate_and_rollback_revisions_are_not_reapplied() {
  moto::test::reset_phone_nav_bridge_probe();
  moto_ui_layout_store::factory_reset();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);
  bridge.restore_layout();
  const int baseline = moto::test::phone_nav_bridge_layout_apply_count();

  CHECK(bridge.on_message(make_message(moto::ble::Message{layout_fixture(9)}, 1)) ==
        moto::ble::AckStatus::Ok);
  wait_for_layout_debounce();
  bridge.apply_pending_layout();
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() == baseline + 1);

  // A retransmission of the revision already in effect is answered without
  // touching flash or the renderer (§5.6).
  CHECK(bridge.on_message(make_message(moto::ble::Message{layout_fixture(9)}, 2)) ==
        moto::ble::AckStatus::Duplicate);
  // Revision 0 is not a valid layout version either.
  CHECK(bridge.on_message(make_message(moto::ble::Message{layout_fixture(0)}, 3)) ==
        moto::ble::AckStatus::Duplicate);
  // An older revision never rolls the device back (§6.5).
  CHECK(bridge.on_message(make_message(moto::ble::Message{layout_fixture(4)}, 4)) ==
        moto::ble::AckStatus::Duplicate);
  wait_for_layout_debounce();
  bridge.apply_pending_layout();
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() == baseline + 1);
  moto_ui_layout_t stored{};
  std::uint16_t stored_revision = 0;
  CHECK(moto_ui_layout_store::load(&stored, &stored_revision));
  CHECK(stored_revision == 9);
}

void test_layout_burst_coalesces_into_one_final_write() {
  moto::test::reset_phone_nav_bridge_probe();
  moto_ui_layout_store::factory_reset();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);
  bridge.restore_layout();

  // Two pushes inside the debounce window: only the final version is applied
  // and written, and the first one is never persisted (§5.6).
  CHECK(bridge.on_message(make_message(moto::ble::Message{layout_fixture(20)}, 1)) ==
        moto::ble::AckStatus::Ok);
  CHECK(bridge.on_message(make_message(moto::ble::Message{layout_fixture(21)}, 2)) ==
        moto::ble::AckStatus::Ok);
  bridge.apply_pending_layout();
  moto_ui_layout_t stored{};
  std::uint16_t stored_revision = 0;
  CHECK(!moto_ui_layout_store::load(&stored, &stored_revision));

  wait_for_layout_debounce();
  bridge.apply_pending_layout();
  CHECK(moto_ui_layout_store::load(&stored, &stored_revision));
  CHECK(stored_revision == 21);
  CHECK(moto::test::phone_nav_bridge_last_applied_layout().zoom_percent == 150);
}

void test_layout_restores_the_persisted_record_instead_of_the_default() {
  moto::test::reset_phone_nav_bridge_probe();
  moto_ui_layout_store::factory_reset();
  // A layout written by an earlier power cycle: the next boot must show it
  // rather than the built-in default (§6.5).
  moto_ui_layout_t previous{};
  previous.north_up = 0;
  previous.music_page_enabled = 0;
  previous.map_visible = 1;
  previous.theme_id = static_cast<std::uint8_t>(moto::ble::UiThemeId::HighContrast);
  previous.map_height_px = 400;
  previous.map_opacity = 55;
  previous.route_r = 0xAA;
  previous.route_g = 0xBB;
  previous.route_b = 0xCC;
  previous.route_width_px = 4;
  previous.route_opacity = 100;
  previous.street_r = 0x11;
  previous.street_g = 0x22;
  previous.street_b = 0x33;
  previous.street_width_px = 2;
  previous.street_opacity = 20;
  previous.building_opacity = 10;
  previous.layer_mask = moto::ble::UiLayerBuildings;
  previous.zoom_percent = 60;
  previous.component_count = 1;
  previous.components[0] = {MOTO_UI_COMPONENT_SPEED, 1, MOTO_UI_SIZE_L, 100,
                            5, 6, 0, 0};
  CHECK(moto_ui_layout_store::save(&previous, 33));

  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);
  bridge.restore_layout();
  const moto_ui_layout_t restored =
      moto::test::phone_nav_bridge_last_applied_layout();
  CHECK(std::memcmp(&restored, &previous, sizeof(restored)) == 0);
  CHECK(restored.north_up == 0);
  CHECK(restored.map_height_px == 400);
  CHECK(restored.zoom_percent == 60);

  // The restored revision is in effect, so a push repeating it is answered as a
  // duplicate and the record is left alone (§5.4/§5.6).
  const int baseline = moto::test::phone_nav_bridge_layout_apply_count();
  CHECK(bridge.on_message(make_message(moto::ble::Message{layout_fixture(33)}, 2)) ==
        moto::ble::AckStatus::Duplicate);
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() == baseline);
}

void test_layout_malformed_payload_is_rejected_without_touching_the_screen() {
  moto::test::reset_phone_nav_bridge_probe();
  moto_ui_layout_store::factory_reset();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);
  bridge.restore_layout();
  const int baseline = moto::test::phone_nav_bridge_layout_apply_count();
  const moto_ui_layout_t before = moto::test::phone_nav_bridge_last_applied_layout();

  moto::ble::ReassembledMessage message =
      make_message(moto::ble::Message{layout_fixture(40)}, 1);
  // Component records start after the 22-byte header and are 10 bytes each;
  // span_deg is the last byte. An arc span of 100° is not a legal value, so the
  // whole layout must be refused (§5.3), not partially applied.
  const std::size_t span_offset = 22 + 7 * 10 + 9;
  CHECK(message.payload.size() > span_offset);
  CHECK(message.payload[span_offset] == 90);
  message.payload[span_offset] = 100;
  CHECK(bridge.on_message(message) == moto::ble::AckStatus::Failed);
  wait_for_layout_debounce();
  bridge.apply_pending_layout();
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() == baseline);
  CHECK(std::memcmp(&before, &moto::test::phone_nav_bridge_last_applied_layout(),
                    sizeof(before)) == 0);
  moto_ui_layout_t stored{};
  std::uint16_t stored_revision = 0;
  CHECK(!moto_ui_layout_store::load(&stored, &stored_revision));

  // Trailing bytes are rejected as well, so a newer protocol revision cannot be
  // silently reinterpreted by this firmware (§5.3).
  moto::ble::ReassembledMessage trailing =
      make_message(moto::ble::Message{layout_fixture(41)}, 2);
  trailing.payload.push_back(0);
  CHECK(bridge.on_message(trailing) == moto::ble::AckStatus::Failed);
  CHECK(moto::test::phone_nav_bridge_layout_apply_count() == baseline);
}

// Reproduces the one genuine race in the layout path: a phone push that lands
// after the bridge has read the pending layout but before its flash write has
// finished. The newer layout must survive, because the bridge has already
// acknowledged it (§5.6) — dropping it would be a silent lost update.
struct PendingPushHook {
  PhoneNavBridge* bridge = nullptr;
  moto::ble::UiLayout layout{};
  std::uint16_t sequence = 500;
  bool fired = false;
};

PendingPushHook* pending_push_hook = nullptr;

void fire_pending_push_hook() {
  if (pending_push_hook == nullptr || pending_push_hook->fired) {
    return;
  }
  pending_push_hook->fired = true;
  pending_push_hook->bridge->on_message(
      make_message(moto::ble::Message{pending_push_hook->layout},
                   pending_push_hook->sequence));
}

void test_layout_pushed_during_the_flash_write_is_not_lost() {
  moto::test::reset_phone_nav_bridge_probe();
  moto_ui_layout_store::factory_reset();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);
  bridge.restore_layout();

  CHECK(bridge.on_message(make_message(moto::ble::Message{layout_fixture(60)}, 1)) ==
        moto::ble::AckStatus::Ok);
  wait_for_layout_debounce();

  // The newer push differs visibly, so the applied layout identifies which
  // revision really won.
  moto::ble::UiLayout newer = layout_fixture(61);
  newer.zoom_percent = 90;
  newer.map_height_px = 200;
  PendingPushHook hook;
  hook.bridge = &bridge;
  hook.layout = newer;
  pending_push_hook = &hook;
  bridge.set_before_layout_persist_hook(fire_pending_push_hook);

  bridge.apply_pending_layout();
  bridge.set_before_layout_persist_hook(nullptr);
  pending_push_hook = nullptr;
  CHECK(hook.fired);

  // The first pass wrote revision 60 while revision 61 arrived. It must have
  // been written (it was already on screen) but it may not be the final record.
  {
    moto_ui_layout_t interim{};
    std::uint16_t interim_revision = 0;
    CHECK(moto_ui_layout_store::load(&interim, &interim_revision));
    CHECK(interim_revision == 60);
  }

  // The superseded push has to be settled on a later frame, not discarded.
  wait_for_layout_debounce();
  bridge.apply_pending_layout();
  const moto_ui_layout_t applied =
      moto::test::phone_nav_bridge_last_applied_layout();
  CHECK(applied.zoom_percent == 90);
  CHECK(applied.map_height_px == 200);
  moto_ui_layout_t stored{};
  std::uint16_t stored_revision = 0;
  CHECK(moto_ui_layout_store::load(&stored, &stored_revision));
  CHECK(stored_revision == 61);
  CHECK(stored.zoom_percent == 90);

  // Now that revision 61 is in effect, repeating it is a duplicate.
  CHECK(bridge.on_message(make_message(moto::ble::Message{newer}, 3)) ==
        moto::ble::AckStatus::Duplicate);
}

void test_idle_snapshot_leaves_navigation_and_clears_the_route() {
  moto::test::reset_phone_nav_bridge_probe();
  // The bridge restores the persisted layout in its constructor, and the host
  // store is process-global. Reset it first, or a layout written by an earlier
  // test (a shorter map viewport) changes what counts as "inside" here.
  moto_ui_layout_store::factory_reset();
  moto::nav::NavPresenter presenter;
  PhoneNavBridge bridge(presenter);
  bridge.install_ui_callbacks();
  bridge.on_link_state(true);

  // A short route just south-east of the origin: all three points stay inside
  // the default 232 px map viewport, so clipping cannot hide the geometry.
  moto::ble::RouteGeometry geometry;
  geometry.route_token = moto::ble::route_token("ble-end-test");
  geometry.route_generation = 1;
  geometry.total_point_count = 3;
  geometry.view_origin = {36'674'773, 117'128'056};
  geometry.points = {
      {36'674'773, 117'128'056},
      {36'674'593, 117'128'392},
      {36'674'413, 117'128'280},
  };

  moto::ble::NavigationSnapshot navigating;
  navigating.state = moto::ble::NavigationState::Navigating;
  navigating.network = moto::ble::NetworkState::Online;
  navigating.flags = moto::ble::NavigationHasDestination |
                     moto::ble::NavigationHasFix |
                     moto::ble::NavigationHasNextManeuver |
                     moto::ble::NavigationHasRouteView;
  navigating.route_token = moto::ble::route_token("ble-end-test");
  navigating.route_generation = 1;
  navigating.heading_cdeg = 9'000;
  navigating.speed_deci_kph = 500;
  navigating.maneuver = moto::ble::Maneuver::Left;
  navigating.distance_to_maneuver_m = 300;
  navigating.remaining_distance_m = 8'200;
  navigating.remaining_duration_s = 1'260;
  navigating.route_progress_m = 2'000;

  CHECK(bridge.on_message(make_message(geometry, 1)) == moto::ble::AckStatus::Ok);
  CHECK(bridge.on_message(make_message(navigating, 2)) == moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_route_point_count() >= 2);
  CHECK(moto::test::phone_nav_bridge_last_distance_to_maneuver_m() == 300);

  // Exiting navigation: the phone pushes the core's post-cancel frame - Idle,
  // no destination, no route-view flag. The device has to leave the navigation
  // presentation instead of holding the last route and maneuver on screen until
  // the BLE link drops (the reported "app exited, display did nothing" bug).
  moto::ble::NavigationSnapshot idle;
  idle.state = moto::ble::NavigationState::Idle;
  idle.network = moto::ble::NetworkState::Online;
  idle.flags = moto::ble::NavigationHasFix;
  idle.route_token = moto::ble::route_token("ble-end-test");
  idle.route_generation = 0;
  idle.heading_cdeg = 9'000;
  CHECK(bridge.on_message(make_message(idle, 3)) == moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_route_point_count() == 0);
  CHECK(moto::test::phone_nav_bridge_last_road_point_count() == 0);
  CHECK(moto::test::phone_nav_bridge_last_distance_to_maneuver_m() == 0);

  // A following snapshot must not resurrect the dropped route just because the
  // token still matches: the geometry is gone, so there is nothing to project.
  CHECK(bridge.on_message(make_message(idle, 4)) == moto::ble::AckStatus::Ok);
  pump(bridge);
  CHECK(moto::test::phone_nav_bridge_last_route_point_count() == 0);
}

}  // namespace

int main() {
  test_sender_is_called_without_bridge_state_lock();
  test_navigation_and_touch_updates_are_serialized();
  test_ble_submission_never_waits_for_lvgl_and_retries_latest_state();
  test_phone_connection_lifecycle_reaches_the_ui();
  test_music_capability_state_and_touch_commands();
  test_demo_uses_the_production_presenter_path();
  test_ios_demo_token_attaches_context_and_live_route_clears_it();
  test_map_scene_atomically_replaces_roads_and_buildings_at_capacity();
  test_imu_presentation_preserves_40hz_phase_and_stale_course_is_ignored();
  test_layout_is_applied_only_after_the_debounce_and_is_persisted();
  test_layout_duplicate_and_rollback_revisions_are_not_reapplied();
  test_layout_burst_coalesces_into_one_final_write();
  test_layout_restores_the_persisted_record_instead_of_the_default();
  test_layout_malformed_payload_is_rejected_without_touching_the_screen();
  test_layout_pushed_during_the_flash_write_is_not_lost();
  test_idle_snapshot_leaves_navigation_and_clears_the_route();

  if (failures != 0) {
    std::cerr << failures << " phone navigation bridge checks failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All phone navigation bridge checks passed\n";
  return EXIT_SUCCESS;
}
