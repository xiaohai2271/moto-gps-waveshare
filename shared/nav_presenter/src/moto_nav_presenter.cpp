#include "moto_nav_presenter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace moto::nav {
namespace {

constexpr double kEarthRadiusM = 6'371'000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kLegacyCanvasSize = 360.0;
constexpr double kUiScale =
    static_cast<double>(MOTO_UI_CANVAS_WIDTH) / kLegacyCanvasSize;
constexpr double kBasePixelsPerMeter = 0.44 * kUiScale;
// The rider sits 196/232 of the way down the default 232 px map viewport.
// Follow the configured viewport height so shrinking the map keeps the same
// forward-visibility ratio (docs/NAV_LAYOUT_EDITOR_PLAN.md §4.8).
constexpr double kVehicleForwardRatio = 196.0 / 232.0;

double radians(double degrees) { return degrees * kPi / 180.0; }

moto_ui_mode_t map_mode(const NavSnapshot& snapshot) {
  switch (snapshot.state) {
    case NavState::Idle:
    case NavState::Acquiring:
    case NavState::Planning:
      return MOTO_UI_ACQUIRING_FIX;
    case NavState::Navigating:
      return snapshot.network == NetworkState::Offline
                 ? MOTO_UI_OFFLINE
                 : MOTO_UI_NAVIGATING;
    case NavState::Rerouting:
      return MOTO_UI_REROUTING;
    case NavState::Arrived:
      return MOTO_UI_ARRIVED;
  }
  return MOTO_UI_ACQUIRING_FIX;
}

moto_ui_page_t map_page(DisplayPage page) {
  switch(page) {
    case DisplayPage::Navigation: return MOTO_UI_PAGE_NAVIGATION;
    case DisplayPage::Speed: return MOTO_UI_PAGE_SPEED;
    case DisplayPage::Compass: return MOTO_UI_PAGE_COMPASS;
    case DisplayPage::Music: return MOTO_UI_PAGE_MUSIC;
  }
  return MOTO_UI_PAGE_NAVIGATION;
}

moto_maneuver_t map_maneuver(ManeuverType maneuver) {
  switch (maneuver) {
    case ManeuverType::SlightLeft:
      return MOTO_MANEUVER_SLIGHT_LEFT;
    case ManeuverType::Left:
    case ManeuverType::SharpLeft:
      return MOTO_MANEUVER_LEFT;
    case ManeuverType::SlightRight:
      return MOTO_MANEUVER_SLIGHT_RIGHT;
    case ManeuverType::Right:
    case ManeuverType::SharpRight:
    case ManeuverType::Exit:
      return MOTO_MANEUVER_RIGHT;
    case ManeuverType::UTurnLeft:
    case ManeuverType::UTurnRight:
      return MOTO_MANEUVER_UTURN;
    case ManeuverType::Roundabout:
      return MOTO_MANEUVER_ROUNDABOUT;
    case ManeuverType::Arrive:
      return MOTO_MANEUVER_ARRIVE;
    case ManeuverType::Unknown:
    case ManeuverType::Continue:
      return MOTO_MANEUVER_STRAIGHT;
  }
  return MOTO_MANEUVER_STRAIGHT;
}

moto_traffic_t map_traffic(TrafficLevel traffic) {
  switch (traffic) {
    case TrafficLevel::FreeFlow:
      return MOTO_TRAFFIC_CLEAR;
    case TrafficLevel::Slow:
      return MOTO_TRAFFIC_SLOW;
    case TrafficLevel::Congested:
      return MOTO_TRAFFIC_CONGESTED;
    case TrafficLevel::Severe:
      return MOTO_TRAFFIC_SEVERE;
    case TrafficLevel::Unknown:
      return MOTO_TRAFFIC_UNKNOWN;
  }
  return MOTO_TRAFFIC_UNKNOWN;
}

template <typename Integer>
Integer rounded_saturated(double value) {
  static_assert(std::numeric_limits<Integer>::is_integer,
                "destination must be an integer");
  if (!std::isfinite(value) || value <= 0.0) {
    return 0;
  }
  const double maximum =
      static_cast<double>(std::numeric_limits<Integer>::max());
  if (value >= maximum) {
    return std::numeric_limits<Integer>::max();
  }
  return static_cast<Integer>(std::floor(value + 0.5));
}

std::uint8_t progress_percent(const NavSnapshot& snapshot) {
  if (snapshot.state == NavState::Arrived) {
    return 100;
  }
  if (!std::isfinite(snapshot.total_distance_m) ||
      snapshot.total_distance_m <= 0.0 ||
      !std::isfinite(snapshot.route_progress_m)) {
    return 0;
  }
  const double percent = std::clamp(
      snapshot.route_progress_m / snapshot.total_distance_m * 100.0,
      0.0, 100.0);
  return rounded_saturated<std::uint8_t>(percent);
}

std::uint16_t normalized_heading(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  double normalized = std::fmod(static_cast<double>(value), 360.0);
  if (normalized < 0.0) {
    normalized += 360.0;
  }
  return static_cast<std::uint16_t>(std::lround(normalized)) % 360;
}

std::uint32_t route_identity(const std::string& route_id) {
  if (route_id.empty()) return 0;
  std::uint32_t hash = 2'166'136'261U;
  for (const unsigned char byte : route_id) {
    hash ^= byte;
    hash *= 16'777'619U;
  }
  // Zero is reserved for the no-route state.
  return hash == 0 ? 1 : hash;
}

struct MapTransform {
  double origin_latitude_deg = 0.0;
  double origin_longitude_deg = 0.0;
  double longitude_pixels_per_degree = 1.0;
  double latitude_pixels_per_degree = 1.0;
  float vehicle_x = MOTO_UI_CANVAS_WIDTH / 2.0F;
  float vehicle_y = 0.0F;
  float min_y = 0.0F;
  float max_y = static_cast<float>(MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT);
  float sin_heading = 0.0F;
  float cos_heading = 1.0F;
};

MapTransform map_transform(const NavSnapshot& snapshot, bool north_up,
                           int map_height_px, int zoom_percent) {
  const double finite_heading =
      std::isfinite(snapshot.heading_deg) ? snapshot.heading_deg : 0.0;
  // North-up leaves the map fixed; the vehicle marker carries the real phone
  // heading instead. Heading-up rotates the whole map under a fixed marker.
  const double heading = north_up ? 0.0 : radians(finite_heading);
  const double zoom = std::clamp(zoom_percent, 50, 200) / 100.0;
  const double pixels_per_meter = kBasePixelsPerMeter * zoom;
  const double height = std::clamp(map_height_px,
                                   MOTO_UI_LAYOUT_MIN_MAP_HEIGHT,
                                   MOTO_UI_LAYOUT_MAX_MAP_HEIGHT);
  return {
      snapshot.route_view_origin.latitude_deg,
      snapshot.route_view_origin.longitude_deg,
      std::cos(radians(snapshot.route_view_origin.latitude_deg)) *
          (kPi / 180.0 * kEarthRadiusM * pixels_per_meter),
      kPi / 180.0 * kEarthRadiusM * pixels_per_meter,
      static_cast<float>(MOTO_UI_CANVAS_WIDTH / 2.0),
      static_cast<float>(height * kVehicleForwardRatio),
      0.0F,
      static_cast<float>(height),
      static_cast<float>(std::sin(heading)),
      static_cast<float>(std::cos(heading)),
  };
}

bool point_inside_viewport(const moto_ui_point_t& point,
                           const MapTransform& transform) {
  // The map is a top-anchored viewport. Points below it (or above the screen)
  // are clipped so the firmware never renders a fake line along the boundary.
  return point.y >= 0 &&
         static_cast<float>(point.y) < transform.max_y &&
         point.x >= -2'000 && point.x <= 2'000;
}

moto_ui_point_t project_map_point(const Gcj02Point& point,
                                  const MapTransform& transform) {
  // Subtract geographic doubles before converting to screen-space floats.
  // Casting lat/lon first would lose metres of precision; after subtraction,
  // float is precise well below one pixel and avoids software-double rotation
  // for hundreds of points on the ESP32-S3's single-precision FPU.
  const float east = static_cast<float>(
      (point.longitude_deg - transform.origin_longitude_deg) *
      transform.longitude_pixels_per_degree);
  const float north = static_cast<float>(
      (point.latitude_deg - transform.origin_latitude_deg) *
      transform.latitude_pixels_per_degree);

  // Heading-up transform: the rider remains fixed and every real street
  // translates/rotates together under it, as on an automotive minimap.
  const float right = east * transform.cos_heading -
                       north * transform.sin_heading;
  const float forward = east * transform.sin_heading +
                         north * transform.cos_heading;
  const float x = transform.vehicle_x + right;
  const float y = transform.vehicle_y - forward;
  return {
      static_cast<std::int16_t>(
          std::clamp(std::lround(x), -2'000L, 2'000L)),
      static_cast<std::int16_t>(
          std::clamp(std::lround(y), -2'000L, 2'000L)),
  };
}

void project_route_view(const NavSnapshot& snapshot,
                        const MapTransform& transform,
                        moto_ui_state_t& output) {
  output.route_point_count = 0;
  if (!snapshot.has_route_view || snapshot.route_view_point_count < 2) {
    return;
  }

  const std::size_t count = std::min<std::size_t>(
      snapshot.route_view_point_count, MOTO_UI_ROUTE_POINT_CAPACITY);
  std::uint8_t accepted = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const moto_ui_point_t projected =
        project_map_point(snapshot.route_view_points[i], transform);
    if (!point_inside_viewport(projected, transform)) continue;
    output.route_points[accepted++] = projected;
  }
  // A clipped route that no longer reaches the rider is not navigable
  // geometry; hide it rather than drawing a detached fragment.
  output.route_point_count = accepted >= 2 ? accepted : 0;
}

void project_road_context(const NavSnapshot& snapshot,
                          const MapTransform& transform,
                          moto_ui_state_t& output) {
  output.road_point_count = 0;
  output.road_polyline_count = 0;
  if (!snapshot.has_route_view || !snapshot.has_road_context ||
      snapshot.road_context_point_count < 2 ||
      snapshot.road_context_polyline_count == 0) {
    return;
  }

  const std::size_t polyline_count = std::min<std::size_t>(
      snapshot.road_context_polyline_count,
      MOTO_UI_ROAD_POLYLINE_CAPACITY);
  std::uint8_t point_count = 0;
  std::uint8_t accepted_polylines = 0;
  for (std::size_t index = 0; index < polyline_count; ++index) {
    const RoadContextPolylineSpan& span =
        snapshot.road_context_polylines[index];
    const std::size_t first = span.first_point_index;
    const std::size_t count = span.point_count;
    if (count < 2 || first >= snapshot.road_context_point_count ||
        count > snapshot.road_context_point_count - first) {
      continue;
    }
    // Clip a polyline into its visible runs so a street that exits the map
    // viewport never gains a synthetic boundary segment.
    std::uint8_t run_start = point_count;
    std::uint8_t run_count = 0;
    for (std::size_t point_index = 0; point_index < count; ++point_index) {
      const moto_ui_point_t projected = project_map_point(
          snapshot.road_context_points[first + point_index], transform);
      const bool visible = point_inside_viewport(projected, transform);
      if (!visible) {
        if (run_count >= 2 && accepted_polylines <
                                  MOTO_UI_ROAD_POLYLINE_CAPACITY) {
          output.road_polylines[accepted_polylines++] = {
              run_start, run_count,
              static_cast<std::uint8_t>(span.road_class)};
        }
        run_start = point_count;
        run_count = 0;
        continue;
      }
      if (point_count >= MOTO_UI_ROAD_POINT_CAPACITY) break;
      output.road_points[point_count++] = projected;
      ++run_count;
    }
    if (run_count >= 2 &&
        accepted_polylines < MOTO_UI_ROAD_POLYLINE_CAPACITY) {
      output.road_polylines[accepted_polylines++] = {
          run_start, run_count, static_cast<std::uint8_t>(span.road_class)};
    }
    if (point_count >= MOTO_UI_ROAD_POINT_CAPACITY) break;
  }
  output.road_point_count = point_count;
  output.road_polyline_count = accepted_polylines;
}

void project_building_context(const NavSnapshot& snapshot,
                              const MapTransform& transform,
                              moto_ui_state_t& output) {
  output.building_point_count = 0;
  output.building_footprint_count = 0;
  if (!snapshot.has_route_view || !snapshot.has_building_context ||
      snapshot.building_context_point_count < 3 ||
      snapshot.building_context_footprint_count == 0) {
    return;
  }

  const std::size_t footprint_count = std::min<std::size_t>(
      snapshot.building_context_footprint_count,
      MOTO_UI_BUILDING_FOOTPRINT_CAPACITY);
  std::uint8_t point_count = 0;
  std::uint8_t accepted_footprints = 0;
  for (std::size_t index = 0; index < footprint_count; ++index) {
    const BuildingContextFootprintSpan& span =
        snapshot.building_context_footprints[index];
    const std::size_t first = span.first_point_index;
    const std::size_t count = span.point_count;
    if (count < 3 || first >= snapshot.building_context_point_count ||
        count > snapshot.building_context_point_count - first) {
      continue;
    }
    if (point_count + count > MOTO_UI_BUILDING_POINT_CAPACITY) break;
    const std::uint8_t footprint_start = point_count;
    bool all_visible = true;
    for (std::size_t point_index = 0; point_index < count; ++point_index) {
      const moto_ui_point_t projected = project_map_point(
          snapshot.building_context_points[first + point_index], transform);
      if (!point_inside_viewport(projected, transform)) {
        all_visible = false;
        break;
      }
      output.building_points[point_count++] = projected;
    }
    if (!all_visible) {
      // A partial footprint would render as a wrong polygon; drop it whole.
      point_count = footprint_start;
      continue;
    }
    output.building_footprints[accepted_footprints++] = {
        footprint_start, static_cast<std::uint8_t>(count),
        static_cast<std::uint8_t>(span.building_class)};
  }
  output.building_point_count = point_count;
  output.building_footprint_count = accepted_footprints;
}

}  // namespace

NavPresenter::NavPresenter() { update(NavSnapshot{}); }

void NavPresenter::update(const NavSnapshot& snapshot) {
  road_name_.clear();
  next_road_name_.clear();
  if (snapshot.has_next_maneuver) {
    // NavSnapshot currently exposes the road associated with the next
    // maneuver, but no separate current-road field. Keep two owned copies so
    // both C pointers have an explicit, stable lifetime. A future current-road
    // field can replace road_name_ without changing moto_ui_state_t.
    road_name_ = snapshot.next_maneuver.road_name;
    next_road_name_ = snapshot.next_maneuver.road_name.empty()
                          ? snapshot.next_maneuver.instruction
                          : snapshot.next_maneuver.road_name;
  }

  ui_state_ = {};
  ui_state_.page = map_page(snapshot.display_page);
  ui_state_.mode = map_mode(snapshot);
  ui_state_.maneuver =
      snapshot.has_next_maneuver
          ? map_maneuver(snapshot.next_maneuver.type)
          : (snapshot.state == NavState::Arrived ? MOTO_MANEUVER_ARRIVE
                                                 : MOTO_MANEUVER_STRAIGHT);
  ui_state_.traffic = map_traffic(snapshot.traffic_ahead);
  ui_state_.distance_to_maneuver_m =
      rounded_saturated<std::uint32_t>(
          snapshot.distance_to_next_maneuver_m);
  ui_state_.remaining_distance_m =
      rounded_saturated<std::uint32_t>(snapshot.remaining_distance_m);
  ui_state_.remaining_time_s = snapshot.remaining_duration_s;
  ui_state_.route_progress_percent = progress_percent(snapshot);
  ui_state_.gps_accuracy_m =
      snapshot.has_usable_fix
          ? rounded_saturated<std::uint8_t>(snapshot.horizontal_accuracy_m)
          : 0;
  ui_state_.online = snapshot.network == NetworkState::Online ? 1 : 0;
  ui_state_.has_destination = snapshot.has_destination ? 1 : 0;
  ui_state_.route_request_in_flight =
      snapshot.route_request_in_flight ? 1 : 0;
  ui_state_.route_identity = route_identity(snapshot.route_id);
  ui_state_.route_generation = snapshot.route_generation;
  ui_state_.map_scene_revision = snapshot.map_scene_revision;
  ui_state_.speed_kph = rounded_saturated<std::uint16_t>(
      static_cast<double>(snapshot.speed_mps) * 3.6);
  ui_state_.speed_limit_kph = snapshot.speed_limit_kph;
  ui_state_.heading_deg = normalized_heading(snapshot.heading_deg);
  // Route, roads and buildings share one rider-centred transform. Besides
  // guaranteeing exact layer alignment, this avoids repeating trigonometry
  // hundreds of times on every high-rate IMU frame.
  const MapTransform transform =
      map_transform(snapshot, north_up_, map_height_px_, zoom_percent_);
  project_route_view(snapshot, transform, ui_state_);
  project_road_context(snapshot, transform, ui_state_);
  project_building_context(snapshot, transform, ui_state_);
  ui_state_.road_name = road_name_.empty() ? nullptr : road_name_.c_str();
  ui_state_.next_road_name =
      next_road_name_.empty() ? nullptr : next_road_name_.c_str();
}

void NavPresenter::set_view_config(bool north_up, int map_height_px,
                                   int zoom_percent) {
  north_up_ = north_up;
  map_height_px_ = std::clamp(map_height_px,
                              static_cast<int>(MOTO_UI_LAYOUT_MIN_MAP_HEIGHT),
                              static_cast<int>(MOTO_UI_LAYOUT_MAX_MAP_HEIGHT));
  zoom_percent_ = std::clamp(zoom_percent, 50, 200);
}

const moto_ui_state_t& NavPresenter::ui_state() const { return ui_state_; }

void NavPresenter::apply_to_lvgl() const {
  moto_nav_ui_set_state(&ui_state_);
}

}  // namespace moto::nav
