#include "phone_nav_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <type_traits>
#include <utility>

#include "board_port.h"
#include "demo_fixture/jinan_big_data_demo.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "moto_nav_ui.h"
#include "ui_layout_store.h"
#include "wifi_health_server.h"

namespace {
constexpr char kTag[] = "phone_nav";
constexpr double kDemoEarthRadiusM = 6'371'000.0;
constexpr double kPi = 3.14159265358979323846;
// The built-in ride begins at the real mapped driveway beside the D building.
// At 12 m/s the first geometry-derived turn arrives after about 6.6 seconds,
// so a physical-device check sees both the road context and a maneuver quickly.
constexpr double kDemoRouteStartM = 0.0;
constexpr double kDemoSpeedMps = 12.0;
// A phone push is accepted immediately but only applied and persisted once it
// has been stable for this long (plan §5.6: NVS wear protection). A second
// push inside the window simply replaces the pending layout.
constexpr std::uint64_t kLayoutDebounceMs = 2'000;
// Every one of the eight editable components the phone can send (plan §5.4).
constexpr std::uint8_t kUiLayoutSupportedMask = 0xFFU;

struct DemoPose {
  moto::nav::Gcj02Point point{};
  float heading_deg = 0.0F;
};

double demo_geo_distance(const moto::nav::Gcj02Point& from,
                         const moto::nav::Gcj02Point& to) {
  const double latitude =
      (from.latitude_deg + to.latitude_deg) * 0.5 * kPi / 180.0;
  const double north =
      (to.latitude_deg - from.latitude_deg) * kPi / 180.0 *
      kDemoEarthRadiusM;
  const double east =
      (to.longitude_deg - from.longitude_deg) * kPi / 180.0 *
      std::cos(latitude) * kDemoEarthRadiusM;
  return std::hypot(east, north);
}

const std::array<double, moto::demo::jinan_big_data::kRoute.size()>&
demo_cumulative_distance() {
  static const auto cumulative = [] {
    std::array<double, moto::demo::jinan_big_data::kRoute.size()> values{};
    for(std::size_t index = 1; index < values.size(); ++index) {
      values[index] = values[index - 1] + demo_geo_distance(
          moto::demo::jinan_big_data::kRoute[index - 1],
          moto::demo::jinan_big_data::kRoute[index]);
    }
    return values;
  }();
  return cumulative;
}

moto::nav::Gcj02Point demo_route_point(double route_offset_m) {
  const auto& cumulative = demo_cumulative_distance();
  const double geometry_offset = std::clamp(
      route_offset_m, 0.0, cumulative.back());
  auto upper = std::upper_bound(cumulative.begin(), cumulative.end(),
                                geometry_offset);
  if(upper == cumulative.begin()) {
    return moto::demo::jinan_big_data::kRoute.front();
  }
  if(upper == cumulative.end()) {
    return moto::demo::jinan_big_data::kRoute.back();
  }
  const std::size_t end_index =
      static_cast<std::size_t>(upper - cumulative.begin());
  const std::size_t start_index = end_index - 1;
  const double segment = cumulative[end_index] - cumulative[start_index];
  const double fraction = segment > 0.0
                              ? (geometry_offset - cumulative[start_index]) /
                                    segment
                              : 0.0;
  const auto& start = moto::demo::jinan_big_data::kRoute[start_index];
  const auto& end = moto::demo::jinan_big_data::kRoute[end_index];
  return {
      start.latitude_deg + (end.latitude_deg - start.latitude_deg) * fraction,
      start.longitude_deg +
          (end.longitude_deg - start.longitude_deg) * fraction,
  };
}

double demo_route_distance() {
  return demo_cumulative_distance().back();
}

double demo_maneuver_offset(
    const moto::demo::jinan_big_data::ManeuverFixture& maneuver) {
  const auto& cumulative = demo_cumulative_distance();
  const std::size_t index = std::min<std::size_t>(
      maneuver.route_point_index, cumulative.size() - 1);
  return cumulative[index];
}

float demo_bearing(const moto::nav::Gcj02Point& from,
                   const moto::nav::Gcj02Point& to) {
  const double latitude =
      (from.latitude_deg + to.latitude_deg) * 0.5 * kPi / 180.0;
  const double north = to.latitude_deg - from.latitude_deg;
  const double east = (to.longitude_deg - from.longitude_deg) *
                      std::cos(latitude);
  double value = std::atan2(east, north) * 180.0 / kPi;
  if(value < 0.0) value += 360.0;
  return static_cast<float>(value);
}

DemoPose demo_pose(double route_offset_m) {
  constexpr double kTangentHalfWindowM = 11.0;
  const auto before = demo_route_point(route_offset_m - kTangentHalfWindowM);
  const auto after = demo_route_point(route_offset_m + kTangentHalfWindowM);
  return {
      demo_route_point(route_offset_m),
      demo_bearing(before, after),
  };
}

void add_demo_road_context(moto::nav::NavSnapshot& output) {
  static_assert(moto::demo::jinan_big_data::kRoadPoints.size() <=
                moto::nav::kRoadContextPointCapacity);
  static_assert(moto::demo::jinan_big_data::kRoads.size() <=
                moto::nav::kRoadContextPolylineCapacity);
  output.road_context_point_count = static_cast<std::uint8_t>(
      moto::demo::jinan_big_data::kRoadPoints.size());
  std::copy(moto::demo::jinan_big_data::kRoadPoints.begin(),
            moto::demo::jinan_big_data::kRoadPoints.end(),
            output.road_context_points.begin());
  output.road_context_polyline_count = static_cast<std::uint8_t>(
      moto::demo::jinan_big_data::kRoads.size());
  for(std::size_t index = 0;
      index < moto::demo::jinan_big_data::kRoads.size(); ++index) {
    const auto& road = moto::demo::jinan_big_data::kRoads[index];
    output.road_context_polylines[index] = {
        road.first_point_index,
        road.point_count,
        moto::nav::RoadContextClass::Residential,
    };
  }
  output.has_road_context = true;
  output.has_building_context = false;
  output.building_context_point_count = 0;
  output.building_context_footprint_count = 0;
  output.map_scene_revision = 0;
}

void clear_road_context(moto::nav::NavSnapshot& output) {
  output.has_road_context = false;
  output.road_context_point_count = 0;
  output.road_context_polyline_count = 0;
}

void clear_building_context(moto::nav::NavSnapshot& output) {
  output.has_building_context = false;
  output.building_context_point_count = 0;
  output.building_context_footprint_count = 0;
}

void clear_map_context(moto::nav::NavSnapshot& output) {
  clear_road_context(output);
  clear_building_context(output);
  output.map_scene_revision = 0;
}

moto::nav::RoadContextClass map_road_class(
    moto::ble::MapRoadClass road_class) {
  return static_cast<moto::nav::RoadContextClass>(road_class);
}

moto::nav::BuildingContextClass map_building_class(
    moto::ble::MapBuildingClass building_class) {
  return static_cast<moto::nav::BuildingContextClass>(building_class);
}

bool is_ios_demo_route_token(std::uint32_t route_token) {
  static const std::uint32_t kDemoRouteToken = moto::ble::route_token(
      moto::demo::jinan_big_data::kRouteId);
  static const std::uint32_t kReroutedDemoRouteToken =
      moto::ble::route_token(moto::demo::jinan_big_data::kReroutedRouteId);
  return route_token == kDemoRouteToken ||
         route_token == kReroutedDemoRouteToken;
}

moto::nav::NavState map_state(moto::ble::NavigationState state) {
  using In = moto::ble::NavigationState;
  using Out = moto::nav::NavState;
  switch (state) {
    case In::Idle: return Out::Idle;
    case In::Acquiring: return Out::Acquiring;
    case In::Planning: return Out::Planning;
    case In::Navigating: return Out::Navigating;
    case In::Rerouting: return Out::Rerouting;
    case In::Arrived: return Out::Arrived;
  }
  return Out::Idle;
}

moto::nav::NetworkState map_network(moto::ble::NetworkState state) {
  using In = moto::ble::NetworkState;
  using Out = moto::nav::NetworkState;
  switch (state) {
    case In::Offline: return Out::Offline;
    case In::Connecting: return Out::Connecting;
    case In::Online: return Out::Online;
  }
  return Out::Offline;
}

moto::nav::DisplayPage map_page(moto::ble::DisplayPage page) {
  using In = moto::ble::DisplayPage;
  using Out = moto::nav::DisplayPage;
  switch (page) {
    case In::Navigation: return Out::Navigation;
    case In::Speed: return Out::Speed;
    case In::Compass: return Out::Compass;
    case In::Music: return Out::Music;
  }
  return Out::Navigation;
}

moto::ble::DisplayPage map_page(moto_ui_page_t page) {
  switch (page) {
    case MOTO_UI_PAGE_SPEED: return moto::ble::DisplayPage::Speed;
    case MOTO_UI_PAGE_COMPASS: return moto::ble::DisplayPage::Compass;
    case MOTO_UI_PAGE_MUSIC: return moto::ble::DisplayPage::Music;
    case MOTO_UI_PAGE_NAVIGATION:
    case MOTO_UI_PAGE_COUNT:
    default: return moto::ble::DisplayPage::Navigation;
  }
}

moto::nav::DisplayPage map_nav_page(moto_ui_page_t page) {
  return map_page(map_page(page));
}

moto::nav::ManeuverType map_maneuver(moto::ble::Maneuver maneuver) {
  using In = moto::ble::Maneuver;
  using Out = moto::nav::ManeuverType;
  switch (maneuver) {
    case In::Continue: return Out::Continue;
    case In::SlightLeft: return Out::SlightLeft;
    case In::Left: return Out::Left;
    case In::SharpLeft: return Out::SharpLeft;
    case In::UTurnLeft: return Out::UTurnLeft;
    case In::SlightRight: return Out::SlightRight;
    case In::Right: return Out::Right;
    case In::SharpRight: return Out::SharpRight;
    case In::UTurnRight: return Out::UTurnRight;
    case In::Roundabout: return Out::Roundabout;
    case In::Exit: return Out::Exit;
    case In::Arrive: return Out::Arrive;
    case In::Unknown: return Out::Unknown;
  }
  return Out::Unknown;
}

moto::nav::TrafficLevel map_traffic(moto::ble::TrafficLevel level) {
  using In = moto::ble::TrafficLevel;
  using Out = moto::nav::TrafficLevel;
  switch (level) {
    case In::FreeFlow: return Out::FreeFlow;
    case In::Slow: return Out::Slow;
    case In::Congested: return Out::Congested;
    case In::Severe: return Out::Severe;
    case In::Unknown: return Out::Unknown;
  }
  return Out::Unknown;
}

std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
}

bool has_flag(std::uint16_t flags, moto::ble::NavigationFlag flag) {
  return (flags & static_cast<std::uint16_t>(flag)) != 0U;
}

moto_ui_layout_t default_ui_layout() {
  moto_ui_layout_t layout{};
  layout.north_up = 1;
  layout.music_page_enabled = 1;
  layout.map_visible = 1;
  layout.theme_id = static_cast<std::uint8_t>(moto::ble::UiThemeId::Dark);
  layout.map_height_px = MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT;
  layout.map_opacity = 100;
  layout.route_r = 0xFF;
  layout.route_g = 0xFF;
  layout.route_b = 0xFF;
  layout.route_width_px = 6;
  layout.route_opacity = 100;
  layout.street_r = 0x8E;
  layout.street_g = 0x8E;
  layout.street_b = 0x93;
  layout.street_width_px = 3;
  layout.street_opacity = 35;
  layout.building_opacity = 25;
  layout.layer_mask = moto::ble::UiLayerRoads | moto::ble::UiLayerBuildings |
                      moto::ble::UiLayerRoute;
  layout.zoom_percent = 100;
  layout.component_count = MOTO_UI_LAYOUT_COMPONENT_COUNT;

  // §4.10 default coordinates. These are placeholder values tuned on real
  // hardware later; they must stay byte-identical to Android LayoutDefaults so
  // the phone preview and the device agree without any negotiation.
  layout.components[0] = {MOTO_UI_COMPONENT_SPEED, 1, MOTO_UI_SIZE_M, 100,
                          185, 300, 0, 0};
  layout.components[1] = {MOTO_UI_COMPONENT_SPEED_LIMIT, 1, MOTO_UI_SIZE_M,
                          100, 300, 238, 0, 0};
  layout.components[2] = {MOTO_UI_COMPONENT_HEADING, 0, MOTO_UI_SIZE_M, 100,
                          24, 40, 0, 0};
  layout.components[3] = {MOTO_UI_COMPONENT_MANEUVER, 1, MOTO_UI_SIZE_M, 100,
                          29, 220, 0, 0};
  layout.components[4] = {MOTO_UI_COMPONENT_REMAINING_DISTANCE, 1,
                          MOTO_UI_SIZE_M, 90, 120, 390, 0, 0};
  layout.components[5] = {MOTO_UI_COMPONENT_REMAINING_TIME, 1, MOTO_UI_SIZE_M,
                          90, 270, 390, 0, 0};
  layout.components[6] = {MOTO_UI_COMPONENT_PROGRESS_ARC, 1, MOTO_UI_SIZE_M, 80,
                          0, 0, MOTO_UI_ARC_TOP, 120};
  layout.components[7] = {MOTO_UI_COMPONENT_SPEED_ARC, 1, MOTO_UI_SIZE_M, 80,
                          0, 0, MOTO_UI_ARC_BOTTOM, 120};
  return layout;
}

// Phone layout -> renderer layout. The decoder already validated every field,
// so this only transcribes. The renderer's own clamp pass (§6.2) stays as the
// last line of defence for a phone from a newer protocol revision.
moto_ui_layout_t to_renderer_layout(const moto::ble::UiLayout& input) {
  moto_ui_layout_t layout = default_ui_layout();
  layout.north_up =
      (input.flags & moto::ble::UiLayoutNorthUp) != 0U ? 1U : 0U;
  layout.music_page_enabled =
      (input.flags & moto::ble::UiLayoutMusicPageEnabled) != 0U ? 1U : 0U;
  layout.map_visible =
      (input.flags & moto::ble::UiLayoutMapVisible) != 0U ? 1U : 0U;
  layout.theme_id = input.theme;
  layout.map_height_px = input.map_height_px;
  layout.map_opacity = input.map_opacity;
  layout.route_r = input.route_r;
  layout.route_g = input.route_g;
  layout.route_b = input.route_b;
  layout.route_width_px = input.route_width_px;
  layout.route_opacity = input.route_opacity;
  layout.street_r = input.street_r;
  layout.street_g = input.street_g;
  layout.street_b = input.street_b;
  layout.street_width_px = input.street_width_px;
  layout.street_opacity = input.street_opacity;
  layout.building_opacity = input.building_opacity;
  layout.layer_mask = input.layer_mask;
  layout.zoom_percent = input.zoom_percent;
  layout.component_count = static_cast<std::uint8_t>(
      std::min<std::size_t>(input.components.size(),
                            MOTO_UI_LAYOUT_COMPONENT_COUNT));
  for (std::size_t index = 0; index < layout.component_count; ++index) {
    const moto::ble::UiLayoutComponent& source = input.components[index];
    moto_ui_component_layout_t& target = layout.components[index];
    target.id = source.id;
    target.enabled = (source.flags & moto::ble::UiComponentEnabled) != 0U
                         ? 1U
                         : 0U;
    target.size = source.size;
    target.opacity = source.opacity;
    target.x = source.x;
    target.y = source.y;
    target.anchor = source.anchor;
    target.span_deg = source.span_deg;
  }
  return layout;
}
}  // namespace

PhoneNavBridge::PhoneNavBridge(moto::nav::NavPresenter& presenter)
    : presenter_(presenter) {
  // Restoring the persisted layout here keeps the default in place for every
  // caller that constructs the bridge before the shared UI exists. The render
  // path re-applies render_layout_ once the objects are created, so the NVS
  // value survives a boot where the phone never connects.
  restore_layout();
}

void PhoneNavBridge::set_sender(SendCallback callback,
                                void* context) noexcept {
  const std::lock_guard<std::mutex> lock(state_mutex_);
  sender_ = callback;
  sender_context_ = context;
}

void PhoneNavBridge::install_ui_callbacks() {
  moto_nav_ui_set_page_change_callback(page_changed, this);
  moto_nav_ui_set_music_command_callback(music_command, this);
  moto_nav_ui_set_demo_change_callback(demo_changed, this);
}

bool PhoneNavBridge::start_renderer() {
#ifdef ESP_PLATFORM
  if (render_task_handle_.load(std::memory_order_acquire) != nullptr) {
    return true;
  }
  TaskHandle_t created = nullptr;
  // The coalescing renderer runs on the LVGL side and must stay small: task
  // stacks are always allocated from internal RAM, which the WiFi and BLE
  // controllers also need. It only coalesces snapshots and hands them to the
  // LVGL task, so 4 KiB is enough.
  if (xTaskCreate(render_task, "moto_ui_render", 4'096, this, 2,
                  &created) != pdPASS) {
    ESP_LOGE(kTag, "could not start coalescing UI renderer");
    return false;
  }
  render_task_handle_.store(created, std::memory_order_release);
  // Also covers a render request submitted between task creation and handle
  // publication.
  xTaskNotifyGive(created);
#endif
  return true;
}

#ifndef ESP_PLATFORM
void PhoneNavBridge::render_pending_for_test() {
  render_pending();
}
#endif

void PhoneNavBridge::on_link_state(bool active) {
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (link_active_ == active) {
      return;
    }
    link_active_ = active;
    ui_phone_connection_ = active ? MOTO_UI_PHONE_CONNECTING
                                  : MOTO_UI_PHONE_OFFLINE;
    auto& phone_snapshot = phone_snapshot_locked();
    if (active) {
      phone_snapshot.network = moto::nav::NetworkState::Connecting;
    } else {
      phone_session_id_ = 0;
      phone_snapshot.network = moto::nav::NetworkState::Offline;
      phone_snapshot.gnss_stale = true;
      phone_snapshot.has_usable_fix = false;
      phone_snapshot.route_request_in_flight = false;
      phone_snapshot.traffic_request_in_flight = false;
      phone_snapshot.speed_mps = 0.0F;
      geometry_ = {};
      phone_snapshot.has_route_view = false;
      clear_map_context(phone_snapshot);
      heading_fusion_.reset();
      last_motion_present_ms_ = 0;
      // Losing the phone also loses the media capability; the layout switch
      // itself is retained so a reconnect restores the user's choice.
      media_music_available_ = false;
      media_state_ = {};
      media_state_.source_name = "iPhone";
      media_state_.track_title = "等待连接";
    }
  }
  const std::uint32_t flags = static_cast<std::uint32_t>(RenderNavigation) |
      (active ? 0U : static_cast<std::uint32_t>(RenderMedia));
  request_render(flags);
}

void PhoneNavBridge::on_imu_sample(float heading_rate_dps,
                                   std::uint64_t sample_ms) {
  bool should_present = false;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (demo_active_ ||
        !heading_fusion_.integrate(heading_rate_dps, sample_ms)) {
      return;
    }
    snapshot_.heading_deg = heading_fusion_.heading_deg();
    // The QMI8658 runs at 125 Hz. Reproject at the display's unified 40 Hz
    // cadence; requests still coalesce if one physical refresh runs long.
    if (sample_ms - last_motion_present_ms_ >= 25 &&
        (snapshot_.has_route_view ||
         snapshot_.display_page == moto::nav::DisplayPage::Compass)) {
      // Preserve the 25 ms phase. Setting this to sample_ms quantizes every
      // interval to four 8 ms samples (32 ms), silently reducing 40 to 31 Hz.
      last_motion_present_ms_ +=
          ((sample_ms - last_motion_present_ms_) / 25U) * 25U;
      should_present = true;
    }
  }
  if (should_present) present_motion();
}

void PhoneNavBridge::fill_demo_snapshot(moto::nav::NavSnapshot& output,
                                        std::uint64_t elapsed_ms) {
  constexpr std::uint64_t kArrivalHoldMs = 2'200;
  const double route_distance_m = demo_route_distance();
  const std::uint64_t moving_ms = static_cast<std::uint64_t>(
      (route_distance_m - kDemoRouteStartM) /
      kDemoSpeedMps * 1'000.0);
  const std::uint64_t cycle_ms = moving_ms + kArrivalHoldMs;
  const std::uint64_t phase_ms = elapsed_ms % cycle_ms;
  const bool arrived = phase_ms >= moving_ms;
  const double progress = arrived
                              ? route_distance_m
                              : std::min(
                                    route_distance_m,
                                    kDemoRouteStartM +
                                        phase_ms / 1'000.0 * kDemoSpeedMps);
  const DemoPose rider = demo_pose(progress);

  output = {};
  output.state = arrived ? moto::nav::NavState::Arrived
                         : moto::nav::NavState::Navigating;
  output.network = moto::nav::NetworkState::Online;
  output.display_page = moto::nav::DisplayPage::Navigation;
  output.has_destination = true;
  output.has_usable_fix = true;
  output.gnss_stale = false;
  output.speed_mps = arrived ? 0.0F : static_cast<float>(
      kDemoSpeedMps + std::sin(phase_ms / 850.0) * 1.6);
  output.heading_deg = rider.heading_deg;
  output.horizontal_accuracy_m = 3.2F;
  // A clearly visible demo-only value exercises the sign treatment. OSM road
  // geometry does not include a verified speed-limit value for this fixture.
  output.speed_limit_kph = 50;
  output.route_id = moto::demo::jinan_big_data::kRouteId;
  output.route_progress_m = progress;
  output.total_distance_m = route_distance_m;
  output.remaining_distance_m = std::max(
      0.0, route_distance_m - progress);
  output.remaining_duration_s = static_cast<std::uint32_t>(
      std::ceil(output.remaining_distance_m / kDemoSpeedMps));
  output.traffic_ahead = phase_ms % 11'000U > 8'500U
                             ? moto::nav::TrafficLevel::Slow
                             : moto::nav::TrafficLevel::FreeFlow;
  output.route_generation = 1;
  output.now_ms = elapsed_ms;
  output.last_fix_ms = elapsed_ms;
  output.last_traffic_update_ms = elapsed_ms;

  output.has_next_maneuver = !arrived;
  if (!arrived) {
    // Select the first not-yet-passed instruction from the verified OSM
    // route. A maneuver at the route origin only describes the initial road;
    // skip it so the round display opens on the first actionable turn rather
    // than a misleading straight-ahead instruction at 0 m.
    const moto::demo::jinan_big_data::ManeuverFixture* next =
        &moto::demo::jinan_big_data::kManeuvers.back();
    for(std::size_t index = 1;
        index < moto::demo::jinan_big_data::kManeuvers.size(); ++index) {
      const auto& maneuver = moto::demo::jinan_big_data::kManeuvers[index];
      if(demo_maneuver_offset(maneuver) > progress + 0.5) {
        next = &maneuver;
        break;
      }
    }
    output.next_maneuver.id = next->id;
    output.next_maneuver.type = next->type;
    output.next_maneuver.route_offset_m = demo_maneuver_offset(*next);
    output.next_maneuver.road_name = next->road_name;
    output.next_maneuver.instruction = next->instruction;
    output.distance_to_next_maneuver_m = std::max(
        0.0, output.next_maneuver.route_offset_m - progress);
  }

  output.route_view_origin = rider.point;
  output.route_view_point_count =
      static_cast<std::uint8_t>(moto::nav::kRouteViewPointCapacity);
  for (std::size_t i = 0; i < moto::nav::kRouteViewPointCapacity; ++i) {
    // A constant-size 600 m real route window keeps the white selected route
    // stable while the fixed rider advances and the entire map rotates.
    const double sample_distance = std::clamp(
        progress - 52.0 + static_cast<double>(i) * 27.0,
        0.0, route_distance_m);
    output.route_view_points[i] = demo_route_point(sample_distance);
  }
  output.has_route_view = true;
  add_demo_road_context(output);
}

void PhoneNavBridge::update_demo(std::uint64_t now_ms) {
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (!demo_active_) return;
    const std::uint64_t elapsed_ms = now_ms >= demo_started_ms_
                                         ? now_ms - demo_started_ms_
                                         : 0;
    fill_demo_snapshot(snapshot_, elapsed_ms);
  }
  present_navigation();
}

void PhoneNavBridge::set_demo_active(bool active) {
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (active == demo_active_) return;
    demo_active_ = active;
    if (active) {
      snapshot_before_demo_ = snapshot_;
      demo_started_ms_ = monotonic_ms();
      fill_demo_snapshot(snapshot_, 0);
    } else {
      snapshot_ = snapshot_before_demo_;
      demo_started_ms_ = 0;
    }
  }
  present_navigation();
}

void PhoneNavBridge::restore_layout() {
  // NVS is normally initialized by the BLE transport, which starts after this
  // call on the boot path. flash_prepare() is an idempotent nvs_flash_init(),
  // so calling it here makes the persisted layout readable from the very
  // first boot frame instead of falling back to the default (device-verified
  // bug: ESP_ERR_NVS_NOT_INITIALIZED at restore time).
  moto_ui_layout_store::flash_prepare();
  moto_ui_layout_t layout{};
  std::uint16_t revision = 0;
  const bool restored = moto_ui_layout_store::load(&layout, &revision);
  if (!restored) {
    layout = default_layout();
    revision = 0;
  }
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    render_layout_ = layout;
    applied_layout_revision_ = revision;
    layout_nvs_saved_ = restored;
    pending_layout_ = {};
    layout_dirty_ = false;
    layout_dirty_since_ms_ = 0;
  }
  // The projection follows the restored map height/zoom; the render task picks
  // it up on its next frame. Before the UI exists, moto_nav_ui_apply_layout
  // only retains the layout (§6.2), so calling it here is always safe.
  apply_layout_locked(render_layout_);
  ESP_LOGI(kTag, "layout restored: %s revision=%u nvs_saved=%d",
           restored ? "nvs" : "default",
           static_cast<unsigned>(applied_layout_revision_),
           layout_nvs_saved_ ? 1 : 0);
}

void PhoneNavBridge::apply_layout_locked(const moto_ui_layout_t& layout) {
  // The map viewport height and zoom feed the projection (§6.3); the marker
  // rotation mode comes from north_up inside the layout itself.
  presenter_.set_view_config(layout.north_up != 0,
                             static_cast<int>(layout.map_height_px),
                             static_cast<int>(layout.zoom_percent));
  moto_nav_ui_apply_layout(&layout);
}

void PhoneNavBridge::apply_pending_layout() {
  moto::ble::UiLayout pending{};
  std::uint64_t dirty_since_ms = 0;
  bool dirty = false;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    dirty = layout_dirty_;
    dirty_since_ms = layout_dirty_since_ms_;
    if (!dirty) {
      return;
    }
    pending = pending_layout_;
  }
  // Nothing to do until the pushed layout has been stable for the debounce
  // window; a newer push inside the window has already replaced the pending
  // copy, so only the final version is ever applied and written.
  const std::uint64_t now_ms = monotonic_ms();
  const std::uint64_t elapsed =
      dirty_since_ms == 0 || now_ms < dirty_since_ms
          ? 0
          : now_ms - dirty_since_ms;
  if (elapsed < kLayoutDebounceMs) {
    return;
  }

  const moto_ui_layout_t layout = to_renderer_layout(pending);
  // Persisting is the one blocking operation in this path; it stays outside
  // state_mutex_ so BLE decoding is never delayed by a flash write.
  const bool persisted =
      moto_ui_layout_store::save(&layout, pending.layout_revision);
  if (!persisted) {
    // Keep the layout applied (it is already on screen) but report nvs_saved
    // as clear so the phone can retry on the next change.
    ESP_LOGW(kTag, "layout revision %u could not be persisted",
             static_cast<unsigned>(pending.layout_revision));
  }
  apply_layout_locked(layout);

  bool send_state = false;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (layout_dirty_) {
      // A newer push arrived while the flash write was running; leave the
      // pending state alone and let the next frame settle it.
      layout_dirty_ = false;
      layout_dirty_since_ms_ = 0;
      pending_layout_ = {};
      render_layout_ = layout;
      applied_layout_revision_ = pending.layout_revision;
      layout_nvs_saved_ = persisted;
      send_state = true;
    }
  }
  if (send_state) {
    send_ui_layout_state();
  }
}

moto::ble::AckStatus PhoneNavBridge::consume_ui_layout(
    const moto::ble::UiLayout& input) {
  std::uint16_t applied_revision = 0;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    applied_revision = applied_layout_revision_;
  }
  // The phone's revision is monotonic, so a retransmission or a stale push is
  // answered without touching flash or the renderer (§5.6). A rollback to an
  // older revision is ignored as well: the device never undoes a newer layout
  // on its own (§6.5).
  if (input.layout_revision == 0 ||
      input.layout_revision == applied_revision) {
    return moto::ble::AckStatus::Duplicate;
  }
  if (input.layout_revision < applied_revision) {
    return moto::ble::AckStatus::Duplicate;
  }

  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    pending_layout_ = input;
    if (!layout_dirty_) {
      layout_dirty_since_ms_ = monotonic_ms();
    }
    layout_dirty_ = true;
  }
  ESP_LOGI(kTag, "ui layout revision %u accepted",
           static_cast<unsigned>(input.layout_revision));
  return moto::ble::AckStatus::Ok;
}

void PhoneNavBridge::send_ui_layout_state() {
  moto::ble::UiLayoutState state{};
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    state.layout_revision = applied_layout_revision_;
    state.supported_mask = kUiLayoutSupportedMask;
    state.flags = layout_nvs_saved_ ? moto::ble::UiLayoutNvsSaved : 0U;
  }
  const moto::ble::Message message{state};
  // Sending can block briefly and can cause NimBLE callbacks; never keep
  // state_mutex_ held across this boundary.
  SendCallback sender = nullptr;
  void* sender_context = nullptr;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (sender_ == nullptr || !link_active_) {
      return;
    }
    sender = sender_;
    sender_context = sender_context_;
  }
  sender(message, moto::ble::Urgent, sender_context);
}

void PhoneNavBridge::start_wifi() {
  if (wifi_started_) {
    return;
  }
  if (wifi_.start(wifi_status_changed, this, wifi_scan_done) != ESP_OK) {
    ESP_LOGE(kTag, "wifi provisioning start failed");
    return;
  }
  wifi_started_ = true;
  ESP_LOGI(kTag, "wifi provisioning started");
}

void PhoneNavBridge::wifi_scan_done(void* context) {
  auto* self = static_cast<PhoneNavBridge*>(context);
  std::uint16_t request_id = 0;
  {
    const std::lock_guard<std::mutex> lock(self->state_mutex_);
    request_id = self->wifi_scan_request_id_;
  }
  if (request_id != 0U) {
    self->send_wifi_scan_results(request_id);
  }
}

void PhoneNavBridge::wifi_status_changed(const moto::wifi::Status& status,
                                         void* context) {
  auto* self = static_cast<PhoneNavBridge*>(context);
  // Map the module state onto the wire enums (they are numerically identical).
  moto::ble::WifiStatus message;
  message.state = static_cast<std::uint8_t>(status.state);
  message.rssi_dbm = status.rssi_dbm;
  message.error = static_cast<std::uint8_t>(status.error);
  message.ipv4_le = status.ipv4_le;
  message.ssid = status.ssid;
  ESP_LOGI(kTag, "wifi status: state=%u rssi=%d err=%u ip=%08x",
           static_cast<unsigned>(message.state),
           static_cast<int>(message.rssi_dbm),
           static_cast<unsigned>(message.error),
           static_cast<unsigned>(message.ipv4_le));
  // P4 bring-up endpoint: bind /health once the station gets an address.
  if (status.state == moto::wifi::State::Connected) {
    moto::wifi::start_health_endpoint();
  } else {
    moto::wifi::stop_health_endpoint();
  }
  self->send_wifi_message(moto::ble::Message{message});
}

void PhoneNavBridge::send_wifi_message(const moto::ble::Message& message) {
  SendCallback sender = nullptr;
  void* sender_context = nullptr;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (sender_ == nullptr || !link_active_) {
      return;
    }
    sender = sender_;
    sender_context = sender_context_;
  }
  sender(message, moto::ble::Urgent, sender_context);
}

void PhoneNavBridge::send_wifi_status() {
  const moto::wifi::Status status = wifi_.current_status();
  moto::ble::WifiStatus message;
  message.state = static_cast<std::uint8_t>(status.state);
  message.rssi_dbm = status.rssi_dbm;
  message.error = static_cast<std::uint8_t>(status.error);
  message.ipv4_le = status.ipv4_le;
  message.ssid = status.ssid;
  send_wifi_message(moto::ble::Message{message});
}

moto::ble::AckStatus PhoneNavBridge::consume_wifi_config(
    const moto::ble::WifiConfig& input) {
  moto::wifi::Credentials credentials;
  credentials.ssid = input.ssid;
  credentials.password = input.password;
  credentials.hidden =
      (input.flags & moto::ble::WifiHiddenSsid) != 0U;
  credentials.auto_reconnect =
      (input.flags & moto::ble::WifiAutoReconnect) != 0U;
  // Never log the password.
  ESP_LOGI(kTag, "wifi config: action=%u ssid=%s flags=%02x",
           static_cast<unsigned>(input.action), input.ssid.c_str(),
           static_cast<unsigned>(input.flags));
  esp_err_t result = ESP_OK;
  switch (input.action) {
    case moto::ble::WifiSaveAndConnect:
      result = wifi_.save_and_connect(credentials);
      break;
    case moto::ble::WifiConnectTemporary:
      result = wifi_.connect_temporary(credentials);
      break;
    case moto::ble::WifiDisconnect:
      result = wifi_.disconnect();
      break;
    case moto::ble::WifiForget:
      result = wifi_.forget();
      break;
    default:
      return moto::ble::AckStatus::Failed;
  }
  return result == ESP_OK ? moto::ble::AckStatus::Ok
                          : moto::ble::AckStatus::Failed;
}

moto::ble::AckStatus PhoneNavBridge::consume_wifi_scan_request(
    const moto::ble::WifiScanRequest& input) {
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    wifi_scan_request_id_ = input.request_id;
  }
  if (wifi_.start_scan() != ESP_OK) {
    return moto::ble::AckStatus::InvalidState;
  }
  // Results are delivered asynchronously from the scan-done path.
  return moto::ble::AckStatus::Ok;
}

void PhoneNavBridge::send_wifi_scan_results(std::uint16_t request_id) {
  moto::ble::WifiScanResult result;
  result.request_id = request_id;
  std::array<moto::wifi::WifiProvisioning::ScanAp,
             moto::ble::kMaxWifiScanResults>
      buffer{};
  const std::size_t count = wifi_.copy_scan_results(buffer.data(), buffer.size());
  result.access_points.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    moto::ble::WifiScanAp ap;
    ap.rssi_dbm = buffer[i].rssi_dbm;
    ap.auth = buffer[i].auth;
    ap.ssid = buffer[i].ssid;
    result.access_points.push_back(std::move(ap));
  }
  send_wifi_message(moto::ble::Message{result});
}

moto::ble::AckStatus PhoneNavBridge::on_message(
    const moto::ble::ReassembledMessage& message) {
  const auto decoded = moto::ble::decode_message(
      message.type, moto::ble::ByteView(message.payload));
  if (!decoded.ok()) {
    ESP_LOGW(kTag, "drop %u payload: %s at %u",
             static_cast<unsigned>(message.type),
             moto::ble::to_string(decoded.error),
             static_cast<unsigned>(decoded.offset));
    return moto::ble::AckStatus::Failed;
  }

  return std::visit(
      [this](const auto& value) -> moto::ble::AckStatus {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, moto::ble::ConnectionStatus>) {
          if (value.role != moto::ble::EndpointRole::Phone ||
              value.minimum_version > moto::ble::kProtocolVersion ||
              value.maximum_version < moto::ble::kProtocolVersion) {
            return moto::ble::AckStatus::Unsupported;
          }
          const std::uint32_t required_media =
              moto::ble::CapabilityMediaState |
              moto::ble::CapabilityMusicCommands;
          const bool media = value.state == moto::ble::ConnectionState::Ready &&
              (value.capabilities & required_media) == required_media;
          {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            phone_session_id_ = value.session_id;
            // Only record the media capability here. The user-facing music
            // page switch lives in the UiLayout, and nav_ui combines both, so
            // this bridge no longer overrides the user's choice.
            media_music_available_ = media;
            switch (value.state) {
              case moto::ble::ConnectionState::Ready:
                ui_phone_connection_ = MOTO_UI_PHONE_ONLINE;
                break;
              case moto::ble::ConnectionState::Closing:
                ui_phone_connection_ = MOTO_UI_PHONE_OFFLINE;
                break;
              case moto::ble::ConnectionState::Starting:
              case moto::ble::ConnectionState::Degraded:
                ui_phone_connection_ = MOTO_UI_PHONE_CONNECTING;
              break;
            }
          }
          present_navigation();
          if (value.state == moto::ble::ConnectionState::Ready) {
            // Handshake complete: report the layout in effect so the phone can
            // decide whether it still needs to push one (§5.4). Only the Ready
            // transition emits it, so a repeated status does not spam the
            // display-data queue.
            send_ui_layout_state();
            // Same rationale for WiFi (§5.5): one snapshot per Ready.
            send_wifi_status();
          }
        } else if constexpr (std::is_same_v<T, moto::ble::Heartbeat>) {
          std::uint32_t phone_session_id = 0;
          {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            phone_session_id = phone_session_id_;
          }
          if (phone_session_id != 0 && value.session_id != phone_session_id) {
            return moto::ble::AckStatus::InvalidState;
          }
        } else if constexpr (
            std::is_same_v<T, moto::ble::NavigationSnapshot>) {
          consume_navigation(value);
        } else if constexpr (std::is_same_v<T, moto::ble::RouteGeometry>) {
          return consume_geometry(value);
        } else if constexpr (
            std::is_same_v<T, moto::ble::TrafficDeviation>) {
          consume_traffic(value);
        } else if constexpr (std::is_same_v<T, moto::ble::MediaState>) {
          consume_media(value);
        } else if constexpr (std::is_same_v<T, moto::ble::MapScene>) {
          return consume_map_scene(value);
        } else if constexpr (std::is_same_v<T, moto::ble::UiLayout>) {
          return consume_ui_layout(value);
        } else if constexpr (std::is_same_v<T, moto::ble::WifiConfig>) {
          return consume_wifi_config(value);
        } else if constexpr (std::is_same_v<T, moto::ble::WifiScanRequest>) {
          return consume_wifi_scan_request(value);
        } else if constexpr (std::is_same_v<T, moto::ble::Ack>) {
          ESP_LOGD(kTag, "phone ack sequence=%u command=%u status=%u",
                   value.acknowledged_sequence, value.command_id,
                   static_cast<unsigned>(value.status));
        } else if constexpr (std::is_same_v<T, moto::ble::DeviceCommand>) {
          return moto::ble::AckStatus::Unsupported;
        }
        return moto::ble::AckStatus::Ok;
      },
      decoded.value);
}

void PhoneNavBridge::consume_navigation(
    const moto::ble::NavigationSnapshot& input) {
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    // A valid phone snapshot is always authoritative. Older firmware allowed
    // an accidental long press to leave the visible display in its built-in
    // demo forever while real phone geometry was written to a hidden backup.
    if (demo_active_) {
      snapshot_ = snapshot_before_demo_;
      demo_active_ = false;
      demo_started_ms_ = 0;
    }
    auto& phone_snapshot = phone_snapshot_locked();
    // Receiving a valid navigation snapshot proves that the two-phase
    // protocol session is usable even if a reordered Ready frame was not
    // observed by this bridge callback.
    ui_phone_connection_ = MOTO_UI_PHONE_ONLINE;
    // A phone course is absolute while moving; use it as the low-frequency
    // anchor and preserve the local gyroscope's much faster response between
    // Core Location updates. At walking/standstill speeds Core Location course
    // is commonly stale, so MotionHeadingFusion deliberately does not pull the
    // display back toward it.
    const float phone_speed_mps =
        static_cast<float>(input.speed_deci_kph) / 36.0F;
    const float phone_heading_deg =
        static_cast<float>(input.heading_cdeg) / 100.0F;
    const bool has_usable_fix = has_flag(
        input.flags, moto::ble::NavigationHasFix);
    heading_fusion_.anchor(phone_heading_deg, phone_speed_mps,
                           has_usable_fix && !has_flag(
                               input.flags, moto::ble::NavigationGnssStale));

    phone_snapshot.state = map_state(input.state);
    phone_snapshot.network = map_network(input.network);
    phone_snapshot.display_page = map_page(input.display_page);
    phone_snapshot.has_destination = has_flag(
        input.flags, moto::ble::NavigationHasDestination);
    phone_snapshot.has_usable_fix = has_usable_fix;
    phone_snapshot.gnss_stale = has_flag(
        input.flags, moto::ble::NavigationGnssStale);
    phone_snapshot.off_route = has_flag(
        input.flags, moto::ble::NavigationOffRoute);
    phone_snapshot.route_request_in_flight = has_flag(
        input.flags, moto::ble::NavigationRouteRequestInFlight);
    phone_snapshot.traffic_request_in_flight = has_flag(
        input.flags, moto::ble::NavigationTrafficRequestInFlight);
    phone_snapshot.has_next_maneuver = has_flag(
        input.flags, moto::ble::NavigationHasNextManeuver);
    phone_snapshot.speed_mps = phone_speed_mps;
    phone_snapshot.heading_deg = heading_fusion_.initialized()
                                     ? heading_fusion_.heading_deg()
                                     : phone_heading_deg;
    phone_snapshot.horizontal_accuracy_m =
        static_cast<float>(input.accuracy_dm) / 10.0F;
    phone_snapshot.cross_track_distance_m =
        static_cast<float>(input.cross_track_dm) / 10.0F;
    phone_snapshot.speed_limit_kph = input.speed_limit_kph;
    phone_snapshot.route_progress_m = input.route_progress_m;
    phone_snapshot.total_distance_m = input.total_distance_m;
    phone_snapshot.remaining_distance_m = input.remaining_distance_m;
    phone_snapshot.remaining_duration_s = input.remaining_duration_s;
    phone_snapshot.traffic_ahead = map_traffic(input.traffic);
    phone_snapshot.route_generation = input.route_generation;
    phone_snapshot.now_ms = monotonic_ms();
    phone_snapshot.last_fix_ms =
        phone_snapshot.has_usable_fix ? phone_snapshot.now_ms : 0;

    char route_id[20]{};
    std::snprintf(route_id, sizeof(route_id), "ble-%08lx",
                  static_cast<unsigned long>(input.route_token));
    phone_snapshot.route_id = route_id;

    // The built-in demo predates MapScene and still uses its matching,
    // attributed OSM fixture. Once a live MapScene revision has arrived,
    // ordinary navigation snapshots retain that complete phone-supplied
    // window until it is atomically replaced or the session disconnects.
    if (is_ios_demo_route_token(input.route_token) &&
        phone_snapshot.map_scene_revision == 0) {
      add_demo_road_context(phone_snapshot);
    } else if (phone_snapshot.map_scene_revision == 0) {
      clear_map_context(phone_snapshot);
    }

    phone_snapshot.next_maneuver = {};
    if (phone_snapshot.has_next_maneuver) {
      phone_snapshot.next_maneuver.id = input.maneuver_id;
      phone_snapshot.next_maneuver.type = map_maneuver(input.maneuver);
      phone_snapshot.next_maneuver.route_offset_m =
          static_cast<double>(input.route_progress_m) +
          input.distance_to_maneuver_m;
      phone_snapshot.next_maneuver.road_name = input.road_name;
      phone_snapshot.next_maneuver.instruction = input.instruction;
      phone_snapshot.next_maneuver.roundabout_exit = input.roundabout_exit;
      phone_snapshot.distance_to_next_maneuver_m =
          input.distance_to_maneuver_m;
    } else {
      phone_snapshot.distance_to_next_maneuver_m = 0.0;
    }

    const bool expects_geometry = has_flag(
        input.flags, moto::ble::NavigationHasRouteView);
    if (!expects_geometry || !geometry_.complete ||
        geometry_.route_token != input.route_token ||
        geometry_.route_generation != input.route_generation) {
      phone_snapshot.has_route_view = false;
    } else {
      apply_geometry_locked(phone_snapshot);
    }
  }
  present_navigation();
}

moto::ble::AckStatus PhoneNavBridge::consume_geometry(
    const moto::ble::RouteGeometry& input) {
  if (input.total_point_count > moto::nav::kRouteViewPointCapacity) {
    return moto::ble::AckStatus::Unsupported;
  }

  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (input.chunk_index == 0) {
      geometry_ = {};
      geometry_.active = true;
      geometry_.route_token = input.route_token;
      geometry_.route_generation = input.route_generation;
      geometry_.chunk_count = input.chunk_count;
      geometry_.total_point_count = input.total_point_count;
      geometry_.origin_e6 = input.view_origin;
      geometry_.origin = {
          static_cast<double>(input.view_origin.latitude_e6) / 1'000'000.0,
          static_cast<double>(input.view_origin.longitude_e6) / 1'000'000.0,
      };
    }

    if (!geometry_.active || input.route_token != geometry_.route_token ||
        input.route_generation != geometry_.route_generation ||
        input.chunk_count != geometry_.chunk_count ||
        input.total_point_count != geometry_.total_point_count ||
        input.chunk_index != geometry_.next_chunk ||
        input.first_point_index != geometry_.point_count ||
        !(input.view_origin == geometry_.origin_e6)) {
      geometry_ = {};
      return moto::ble::AckStatus::InvalidState;
    }

    for (const auto& point : input.points) {
      if (geometry_.point_count >= moto::nav::kRouteViewPointCapacity) {
        geometry_ = {};
        return moto::ble::AckStatus::Unsupported;
      }
      geometry_.points[geometry_.point_count++] = {
          static_cast<double>(point.latitude_e6) / 1'000'000.0,
          static_cast<double>(point.longitude_e6) / 1'000'000.0,
      };
    }
    ++geometry_.next_chunk;

    if (geometry_.next_chunk == geometry_.chunk_count) {
      if (geometry_.point_count != geometry_.total_point_count) {
        geometry_ = {};
        return moto::ble::AckStatus::InvalidState;
      }
      geometry_.complete = true;
      geometry_.active = false;
      apply_geometry_locked(phone_snapshot_locked());
    }
  }
  // RouteGeometry is queued immediately before its matching
  // NavigationSnapshot by the iPhone.  Retain the completed geometry here and
  // let that snapshot perform the single full LVGL update for the pair.  A
  // second blocking redraw in the BLE worker halved receive throughput and
  // could eventually fill the bounded RX queue during continuous navigation.
  // The next (5 Hz) snapshot is therefore the presentation boundary.
  return moto::ble::AckStatus::Ok;
}

void PhoneNavBridge::consume_traffic(
    const moto::ble::TrafficDeviation& input) {
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    auto& phone_snapshot = phone_snapshot_locked();
    char expected[20]{};
    std::snprintf(expected, sizeof(expected), "ble-%08lx",
                  static_cast<unsigned long>(input.route_token));
    if (phone_snapshot.route_id != expected ||
        phone_snapshot.route_generation != input.route_generation) {
      return;
    }
    phone_snapshot.last_traffic_update_ms = input.observed_at_ms;
    phone_snapshot.remaining_duration_s = input.remaining_duration_s;
    phone_snapshot.cross_track_distance_m =
        static_cast<float>(input.cross_track_dm) / 10.0F;
    phone_snapshot.off_route = (input.flags & moto::ble::OffRoute) != 0U;
    if ((input.flags & moto::ble::Rerouting) != 0U) {
      phone_snapshot.state = moto::nav::NavState::Rerouting;
    }
    if ((input.flags & moto::ble::RouteInvalidated) != 0U) {
      geometry_ = {};
      phone_snapshot.has_route_view = false;
    }
  }
  present_navigation();
}

void PhoneNavBridge::consume_media(const moto::ble::MediaState& input) {
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    media_state_ = input;
  }
  request_render(RenderMedia);
}

moto::ble::AckStatus PhoneNavBridge::consume_map_scene(
    const moto::ble::MapScene& input) {
  std::size_t road_point_count = 0;
  for (const auto& road : input.roads) {
    road_point_count += road.points.size();
  }
  std::size_t building_point_count = 0;
  for (const auto& building : input.buildings) {
    building_point_count += building.points.size();
  }
  if (input.roads.size() > moto::nav::kRoadContextPolylineCapacity ||
      road_point_count > moto::nav::kRoadContextPointCapacity ||
      input.buildings.size() >
          moto::nav::kBuildingContextFootprintCapacity ||
      building_point_count > moto::nav::kBuildingContextPointCapacity) {
    return moto::ble::AckStatus::Unsupported;
  }

  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    auto& target = phone_snapshot_locked();

    // A MapScene is a complete window replacement. Retransmitted or delayed
    // older windows must not roll the display back after the rider has crossed
    // into a newer offline-map tile.
    if (target.map_scene_revision != 0 &&
        input.scene_revision <= target.map_scene_revision) {
      return moto::ble::AckStatus::Duplicate;
    }

    // The decoder has already validated every class, point count and GCJ-02
    // coordinate. Populate hidden storage first and publish counts/flags last,
    // so observers under state_mutex_ see either the old complete scene or the
    // new complete scene, never a mixture of revisions.
    std::size_t next_road_point = 0;
    std::size_t next_road = 0;
    for (const auto& road : input.roads) {
      const std::size_t first = next_road_point;
      for (const auto& point : road.points) {
        target.road_context_points[next_road_point++] = {
            static_cast<double>(point.latitude_e6) / 1'000'000.0,
            static_cast<double>(point.longitude_e6) / 1'000'000.0,
        };
      }
      target.road_context_polylines[next_road++] = {
          static_cast<std::uint8_t>(first),
          static_cast<std::uint8_t>(road.points.size()),
          map_road_class(road.road_class),
      };
    }

    std::size_t next_building_point = 0;
    std::size_t next_building = 0;
    for (const auto& building : input.buildings) {
      const std::size_t first = next_building_point;
      for (const auto& point : building.points) {
        target.building_context_points[next_building_point++] = {
            static_cast<double>(point.latitude_e6) / 1'000'000.0,
            static_cast<double>(point.longitude_e6) / 1'000'000.0,
        };
      }
      target.building_context_footprints[next_building++] = {
          static_cast<std::uint8_t>(first),
          static_cast<std::uint8_t>(building.points.size()),
          map_building_class(building.building_class),
      };
    }

    target.road_context_point_count =
        static_cast<std::uint8_t>(next_road_point);
    target.road_context_polyline_count =
        static_cast<std::uint8_t>(next_road);
    target.has_road_context = next_road > 0;
    target.building_context_point_count =
        static_cast<std::uint8_t>(next_building_point);
    target.building_context_footprint_count =
        static_cast<std::uint8_t>(next_building);
    target.has_building_context = next_building > 0;
    target.map_scene_revision = input.scene_revision;
  }
  ESP_LOGI(kTag,
           "map scene committed: revision=%lu roads=%u/%u buildings=%u/%u",
           static_cast<unsigned long>(input.scene_revision),
           static_cast<unsigned>(input.roads.size()),
           static_cast<unsigned>(road_point_count),
           static_cast<unsigned>(input.buildings.size()),
           static_cast<unsigned>(building_point_count));
  present_navigation();
  return moto::ble::AckStatus::Ok;
}

void PhoneNavBridge::apply_geometry_locked(moto::nav::NavSnapshot& target) {
  if (!geometry_.complete || target.route_id.empty()) {
    target.has_route_view = false;
    return;
  }
  char expected[20]{};
  std::snprintf(expected, sizeof(expected), "ble-%08lx",
                static_cast<unsigned long>(geometry_.route_token));
  if (target.route_id != expected ||
      target.route_generation != geometry_.route_generation) {
    target.has_route_view = false;
    return;
  }
  target.route_view_origin = geometry_.origin;
  target.route_view_point_count =
      static_cast<std::uint8_t>(geometry_.point_count);
  std::copy_n(geometry_.points.begin(), geometry_.point_count,
              target.route_view_points.begin());
  target.has_route_view = geometry_.point_count >= 2;
}

moto::nav::NavSnapshot& PhoneNavBridge::phone_snapshot_locked() noexcept {
  return demo_active_ ? snapshot_before_demo_ : snapshot_;
}

void PhoneNavBridge::present_navigation() {
  request_render(RenderNavigation);
}

void PhoneNavBridge::present_motion() {
  request_render(RenderMotion);
}

void PhoneNavBridge::request_render(std::uint32_t flags) noexcept {
  pending_render_flags_.fetch_or(flags, std::memory_order_release);
#ifdef ESP_PLATFORM
  const TaskHandle_t task =
      render_task_handle_.load(std::memory_order_acquire);
  if (task != nullptr) {
    xTaskNotifyGive(task);
  }
#endif
}

void PhoneNavBridge::render_pending() {
  const std::uint32_t requested =
      pending_render_flags_.exchange(0, std::memory_order_acq_rel);
  if (requested == 0U) {
    return;
  }

  const bool navigation = (requested & RenderNavigation) != 0U;
  const bool motion = !navigation && (requested & RenderMotion) != 0U;
  const bool media = (requested & RenderMedia) != 0U;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (navigation || motion) {
      render_snapshot_ = snapshot_;
    }
    if (navigation) {
      render_phone_connection_ = ui_phone_connection_;
      render_demo_active_ = demo_active_;
      render_media_music_available_ = media_music_available_;
    }
    if (media) {
      render_media_state_ = media_state_;
    }
  }

  // Projection is the expensive part of a motion frame (hundreds of map
  // points and trigonometry), but it does not call LVGL. Keep it outside the
  // display mutex so the LVGL worker can finish the previous flush in parallel.
  if (navigation || motion) {
    presenter_.update(render_snapshot_);
  }

  // The BLE worker has already returned before this point in firmware. The
  // dedicated low-priority renderer is allowed to wait for an in-flight panel
  // flush without applying backpressure to CoreBluetooth writes.
  if (!board_port_lock(UINT32_MAX)) {
#ifdef ESP_PLATFORM
    // An abnormal BSP failure must not turn into a tight self-notification
    // loop. Retain the request and retry shortly from this task only.
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    request_render(requested);
    return;
  }
  if (navigation) {
    moto_nav_ui_set_phone_connection(render_phone_connection_);
    moto_nav_ui_set_demo_active(render_demo_active_ ? 1U : 0U);
    // Only the media capability is pushed here; the user's music-page switch
    // arrives through moto_nav_ui_apply_layout() and both are ANDed inside.
    moto_nav_ui_set_music_page_enabled(
        render_media_music_available_ ? 1U : 0U);
    // Re-apply the retained layout so a layout pushed before the objects
    // existed, or a recreated page, still shows the phone's arrangement.
    moto_nav_ui_apply_layout(&render_layout_);
    presenter_.apply_to_lvgl();
    // Layout bookkeeping runs on the same 40 Hz path as the display, never on
    // the BLE worker: a push is applied and persisted one frame after it
    // settles, and the resulting UiLayoutState is emitted from here too.
    apply_pending_layout();
  } else if (motion) {
    moto_nav_ui_set_motion_state(&presenter_.ui_state());
  }
  if (media) {
    moto_music_state_t state{};
    state.source_name = render_media_state_.source_name.c_str();
    state.track_title = render_media_state_.track_title.c_str();
    state.artist_name = render_media_state_.artist_name.c_str();
    state.connected =
        (render_media_state_.flags & moto::ble::MediaConnected) != 0U;
    state.playing =
        (render_media_state_.flags & moto::ble::MediaPlaying) != 0U;
    state.like_available =
        (render_media_state_.flags & moto::ble::MediaLikeAvailable) != 0U;
    state.liked =
        (render_media_state_.flags & moto::ble::MediaLiked) != 0U;
    moto_nav_ui_set_music_state(&state);
  }
  board_port_unlock();
}

#ifdef ESP_PLATFORM
void PhoneNavBridge::render_task(void* context) {
  static_cast<PhoneNavBridge*>(context)->run_renderer();
}

void PhoneNavBridge::run_renderer() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    render_pending();
  }
}
#endif

void PhoneNavBridge::page_changed(moto_ui_page_t page, void* context) {
  auto* self = static_cast<PhoneNavBridge*>(context);
  {
    const std::lock_guard<std::mutex> lock(self->state_mutex_);
    self->snapshot_.display_page = map_nav_page(page);
  }
  self->present_navigation();
  self->send_page_command(page);
}

moto_ui_layout_t PhoneNavBridge::default_layout() {
  return default_ui_layout();
}

void PhoneNavBridge::store_layout_to_renderer(const moto_ui_layout_t& layout) {
  // Retained so a later frame can re-apply the exact same layout after the UI
  // is created (plan §6.5: read NVS before the UI exists if necessary).
  moto_nav_ui_apply_layout(&layout);
}

void PhoneNavBridge::music_command(moto_music_command_t command,
                                   void* context) {
  static_cast<PhoneNavBridge*>(context)->send_music_command(command);
}

void PhoneNavBridge::demo_changed(uint8_t enabled, void* context) {
  static_cast<PhoneNavBridge*>(context)->set_demo_active(enabled != 0U);
}

void PhoneNavBridge::send_page_command(moto_ui_page_t page) {
  moto::ble::DeviceCommand command;
  command.kind = moto::ble::DeviceCommandKind::PageSelected;
  command.page = map_page(page);
  send_device_command(command);
}

void PhoneNavBridge::send_music_command(moto_music_command_t command) {
  moto::ble::DeviceCommand output;
  switch (command) {
    case MOTO_MUSIC_PREVIOUS:
      output.kind = moto::ble::DeviceCommandKind::MusicPrevious;
      break;
    case MOTO_MUSIC_TOGGLE_PLAYBACK:
      output.kind = moto::ble::DeviceCommandKind::MusicTogglePlayback;
      break;
    case MOTO_MUSIC_NEXT:
      output.kind = moto::ble::DeviceCommandKind::MusicNext;
      break;
    case MOTO_MUSIC_LIKE:
      output.kind = moto::ble::DeviceCommandKind::MusicLike;
      break;
  }
  output.page = moto::ble::DisplayPage::Music;
  send_device_command(output);
}

void PhoneNavBridge::send_device_command(moto::ble::DeviceCommand command) {
  SendCallback sender = nullptr;
  void* sender_context = nullptr;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (sender_ == nullptr || !link_active_) {
      return;
    }
    sender = sender_;
    sender_context = sender_context_;
    command.command_id = next_command_id_;
    ++next_command_id_;
    if (next_command_id_ == 0) {
      next_command_id_ = 1;
    }
  }
  command.event_time_ms = static_cast<std::uint32_t>(monotonic_ms());
  const moto::ble::Message message = command;
  // Sending can block briefly and can cause Core/NimBLE callbacks. Never keep
  // state_mutex_ held across this boundary.
  sender(message, moto::ble::AckRequested | moto::ble::Urgent,
         sender_context);
}
