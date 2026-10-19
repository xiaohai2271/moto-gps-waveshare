// Deterministic host-side rendering tests for the LVGL navigation UI.
// A headless LVGL display renders the real moto_nav_ui into an RGB565
// framebuffer; frames are hashed to verify bit-exact determinism, clean page
// transitions and label/geometry residue cleanup.

#include "moto_nav_ui.h"

#include <lvgl.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "../../shared/nav_ui/src/accel_filter.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__                             \
                << " CHECK failed: " #condition << '\n';                   \
      ++failures;                                                          \
    }                                                                      \
  } while (false)

constexpr int kWidth = MOTO_UI_CANVAS_WIDTH;
constexpr int kHeight = MOTO_UI_CANVAS_HEIGHT;
constexpr std::size_t kFrameBytes = static_cast<std::size_t>(kWidth) *
                                    static_cast<std::size_t>(kHeight) *
                                (LV_COLOR_DEPTH / 8);

std::vector<std::uint8_t> shadow_frame;
std::uint64_t flush_count = 0;

void flush_cb(lv_display_t* display, const lv_area_t* area,
              std::uint8_t* px_map) {
  // Partial rendering: stitch each flushed area into the shadow frame so a
  // complete deterministic frame is always available for hashing.
  const int bytes_per_pixel = LV_COLOR_DEPTH / 8;
  for (int y = area->y1; y <= area->y2; ++y) {
    const std::size_t row_bytes =
        static_cast<std::size_t>(area->x2 - area->x1 + 1) * bytes_per_pixel;
    const std::size_t src =
        static_cast<std::size_t>(y - area->y1) *
        static_cast<std::size_t>(area->x2 - area->x1 + 1) * bytes_per_pixel;
    const std::size_t dst =
        (static_cast<std::size_t>(y) * kWidth +
         static_cast<std::size_t>(area->x1)) * bytes_per_pixel;
    std::memcpy(shadow_frame.data() + dst, px_map + src, row_bytes);
  }
  ++flush_count;
  lv_display_flush_ready(display);
}

void init_display(int buffer_rows, int extra_pool_bytes) {
  lv_init();
  static std::vector<std::uint32_t> extra_pool;
  if(extra_pool_bytes > 0) {
    extra_pool.resize((static_cast<std::size_t>(extra_pool_bytes) + 3U) / 4U);
    const bool added = lv_mem_add_pool(extra_pool.data(), extra_pool.size() * 4U) != nullptr;
    CHECK(added);
    if(!added) std::exit(EXIT_FAILURE);
  }
  shadow_frame.assign(kFrameBytes, 0);
  // Exercise both small partial strips and the firmware's map-sized buffer.
  static std::vector<std::uint8_t> draw_buffer(
      static_cast<std::size_t>(kWidth) * buffer_rows * (LV_COLOR_DEPTH / 8));
  lv_display_t* display = lv_display_create(kWidth, kHeight);
  lv_display_set_flush_cb(display, flush_cb);
  lv_display_set_buffers(
      display, draw_buffer.data(), nullptr, draw_buffer.size(),
      LV_DISPLAY_RENDER_MODE_PARTIAL);
}

void pump(int cycles = 40) {
  for (int i = 0; i < cycles; ++i) {
    lv_tick_inc(5);
    lv_timer_handler();
  }
}

std::uint64_t frame_hash() {
  std::uint64_t hash = 1'469'598'103'934'665'603ULL;
  for (const std::uint8_t byte : shadow_frame) {
    hash ^= byte;
    hash *= 1'099'511'628'211ULL;
  }
  return hash;
}

struct Frame {
  std::uint64_t hash;
  std::vector<std::uint8_t> bytes;
};

Frame capture() {
  pump();
  return Frame{frame_hash(), shadow_frame};
}

moto_ui_state_t base_state() {
  moto_ui_state_t state{};
  state.page = MOTO_UI_PAGE_NAVIGATION;
  state.mode = MOTO_UI_NAVIGATING;
  state.maneuver = MOTO_MANEUVER_RIGHT;
  state.traffic = MOTO_TRAFFIC_SLOW;
  state.distance_to_maneuver_m = 120;
  state.remaining_distance_m = 1'500;
  state.remaining_time_s = 200;
  state.route_progress_percent = 40;
  state.gps_accuracy_m = 5;
  state.online = 1;
  state.has_destination = 1;
  state.route_identity = 77;
  state.route_generation = 1;
  state.map_scene_revision = 4;
  state.speed_kph = 48;
  state.speed_limit_kph = 50;
  state.heading_deg = 90;
  state.road_name = "Road";
  state.next_road_name = "Next Road";
  return state;
}

void fill_route(moto_ui_state_t& state, int count) {
  count = count < 0 ? MOTO_UI_ROUTE_POINT_CAPACITY : count;
  state.route_point_count = static_cast<std::uint8_t>(count);
  for (int i = 0; i < count; ++i) {
    state.route_points[i] = {static_cast<std::int16_t>(40 + i * 16),
                             static_cast<std::int16_t>(233 + (i % 2) * 40)};
  }
}

void fill_streets(moto_ui_state_t& state) {
  state.road_point_count = 8;
  for (int i = 0; i < 8; ++i) {
    state.road_points[i] = {static_cast<std::int16_t>(10 + i * 55),
                            static_cast<std::int16_t>(30 + i * 60)};
  }
  state.road_polyline_count = 1;
  state.road_polylines[0] = {0, 8, 1};
  state.building_point_count = 6;
  for (int i = 0; i < 6; ++i) {
    state.building_points[i] = {
        static_cast<std::int16_t>(300 + (i % 3) * 40),
        static_cast<std::int16_t>(300 + (i / 3) * 40)};
  }
  state.building_footprint_count = 1;
  state.building_footprints[0] = {0, 6, 1};
}

void test_frames_are_deterministic() {
  // Identical UI state must produce a bit-identical frame every time.
  moto_nav_ui_create();
  moto_nav_ui_set_reduce_motion(1);
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_ONLINE);

  moto_ui_state_t state = base_state();
  fill_route(state, 12);
  fill_streets(state);
  moto_nav_ui_set_state(&state);
  const Frame first = capture();

  moto_nav_ui_set_state(&state);
  const Frame second = capture();
  CHECK(first.hash == second.hash);
  CHECK(first.bytes == second.bytes);

  // Perturb the UI with an unrelated page, then come back: the restored
  // navigation frame must be identical to the original (no residue).
  moto_nav_ui_set_page(MOTO_UI_PAGE_SPEED);
  const Frame speed_page = capture();
  moto_nav_ui_set_page(MOTO_UI_PAGE_NAVIGATION);
  const Frame restored = capture();
  CHECK(restored.hash == first.hash);

  // The speed page must actually render different pixels.
  CHECK(speed_page.hash != first.hash);

  // Determinism holds across repeated renders of the same page as well.
  moto_nav_ui_set_page(MOTO_UI_PAGE_SPEED);
  const Frame speed_again = capture();
  CHECK(speed_again.hash == speed_page.hash);
}

void test_clearing_geometry_leaves_no_residue() {
  // The previous test leaves the speed page selected; navigation geometry only
  // updates while the navigation page is the visible one.
  moto_nav_ui_set_page(MOTO_UI_PAGE_NAVIGATION);
  moto_ui_state_t with_route = base_state();
  fill_route(with_route, 18);
  fill_streets(with_route);
  moto_nav_ui_set_state(&with_route);
  const Frame route_frame = capture();

  moto_ui_state_t without_route = with_route;
  without_route.route_point_count = 0;
  without_route.road_point_count = 0;
  without_route.road_polyline_count = 0;
  without_route.building_point_count = 0;
  without_route.building_footprint_count = 0;
  moto_nav_ui_set_state(&without_route);
  const Frame cleared_frame = capture();
  CHECK(route_frame.hash != cleared_frame.hash);

  // Re-drawing the geometry must restore the exact original frame: nothing
  // was left behind by the clear, and nothing accumulated by the redraw.
  moto_nav_ui_set_state(&with_route);
  const Frame redrawn = capture();
  CHECK(redrawn.hash == route_frame.hash);
}

// ---------------------------------------------------------------------------
// Editable layout engine (plan §4 / §6.2).
//
// apply_layout() is the single entry point that positions, sizes, styles and
// shows/hides the eight editable components. It must never rebuild objects and
// must be safe to call repeatedly with the same or a mutated layout.
// ---------------------------------------------------------------------------

void test_layout_defaults_match_the_documented_baseline() {
  // Before any push the device shows the §4.10 default layout, which mirrors
  // Android LayoutDefaults so an unpushed device matches the editor preview.
  const moto_ui_layout_t* layout = moto_nav_ui_current_layout();
  CHECK(layout != nullptr);
  if (layout == nullptr) return;
  CHECK(layout->north_up == 1);
  CHECK(layout->music_page_enabled == 1);
  CHECK(layout->map_height_px == MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT);
  CHECK(layout->map_opacity == 100);
  CHECK(layout->route_r == 0xFF && layout->route_g == 0xFF &&
        layout->route_b == 0xFF);
  CHECK(layout->route_width_px == 6);
  CHECK(layout->route_opacity == 100);
  CHECK(layout->street_r == 0x8E && layout->street_g == 0x8E &&
        layout->street_b == 0x93);
  CHECK(layout->street_width_px == 3);
  CHECK(layout->street_opacity == 35);
  CHECK(layout->building_opacity == 25);
  CHECK(layout->layer_mask == 0x07);
  CHECK(layout->zoom_percent == 100);
  CHECK(layout->component_count == MOTO_UI_LAYOUT_COMPONENT_COUNT);

  // Every component id must appear exactly once, and the two arcs must dock to
  // opposite sides by default so they never overlap (plan §4.3).
  bool seen[MOTO_UI_LAYOUT_COMPONENT_COUNT + 1] = {false};
  for (std::uint8_t i = 0; i < layout->component_count; ++i) {
    const std::uint8_t id = layout->components[i].id;
    CHECK(id >= 1 && id <= MOTO_UI_LAYOUT_COMPONENT_COUNT);
    if (id >= 1 && id <= MOTO_UI_LAYOUT_COMPONENT_COUNT) {
      CHECK(!seen[id]);
      seen[id] = true;
    }
  }
  const moto_ui_component_layout_t& progress =
      layout->components[MOTO_UI_COMPONENT_PROGRESS_ARC - 1];
  const moto_ui_component_layout_t& speed_arc =
      layout->components[MOTO_UI_COMPONENT_SPEED_ARC - 1];
  CHECK(progress.anchor == MOTO_UI_ARC_TOP);
  CHECK(progress.span_deg == 120);
  CHECK(speed_arc.anchor == MOTO_UI_ARC_BOTTOM);
  CHECK(speed_arc.span_deg == 120);
  CHECK(progress.enabled == 1 && speed_arc.enabled == 1);
  // heading is off by default (plan §4.2); the rest are on.
  CHECK(layout->components[MOTO_UI_COMPONENT_HEADING - 1].enabled == 0);
  CHECK(layout->components[MOTO_UI_COMPONENT_SPEED - 1].enabled == 1);
  CHECK(layout->components[MOTO_UI_COMPONENT_SPEED_LIMIT - 1].enabled == 1);
  CHECK(layout->components[MOTO_UI_COMPONENT_MANEUVER - 1].enabled == 1);
  CHECK(layout->components[MOTO_UI_COMPONENT_REMAINING_DISTANCE - 1].enabled == 1);
  CHECK(layout->components[MOTO_UI_COMPONENT_REMAINING_TIME - 1].enabled == 1);
}

void test_apply_layout_clamps_out_of_range_values() {
  moto_ui_layout_t layout{};
  layout.north_up = 1;
  layout.music_page_enabled = 1;
  // Every field deliberately out of range.
  layout.map_height_px = 900;      // above MAX_MAP_HEIGHT
  layout.map_opacity = 200;        // above 100
  layout.zoom_percent = 999;       // above 200
  layout.route_width_px = 99;      // above 16
  layout.street_width_px = 99;     // above 12
  layout.component_count = MOTO_UI_LAYOUT_COMPONENT_COUNT + 4;
  layout.components[0] = {MOTO_UI_COMPONENT_SPEED, 1, 9, 250, -500, -500, 7, 45};
  layout.components[1] = {MOTO_UI_COMPONENT_PROGRESS_ARC, 1, 9, 0,
                          0, 0, 9, 45};
  layout.components[2] = {MOTO_UI_COMPONENT_SPEED_LIMIT, 1, 9, 5, 10, 10, 0, 0};
  moto_nav_ui_apply_layout(&layout);

  const moto_ui_layout_t* applied = moto_nav_ui_current_layout();
  CHECK(applied != nullptr);
  if (applied == nullptr) return;
  // map_height clamps to the documented range and is forced even.
  CHECK(applied->map_height_px == MOTO_UI_LAYOUT_MAX_MAP_HEIGHT);
  CHECK((applied->map_height_px & 1U) == 0U);
  CHECK(applied->map_opacity == 100);
  CHECK(applied->zoom_percent == 200);
  CHECK(applied->component_count == MOTO_UI_LAYOUT_COMPONENT_COUNT);
  const moto_ui_component_layout_t& speed = applied->components[0];
  CHECK(speed.size == MOTO_UI_SIZE_M);
  CHECK(speed.opacity == 100);        // 250 clamps down to the 100% ceiling
  const moto_ui_component_layout_t& arc = applied->components[1];
  CHECK(arc.anchor == MOTO_UI_ARC_TOP);
  CHECK(arc.span_deg == 120);          // unknown span normalises to 120
  const moto_ui_component_layout_t& limit = applied->components[2];
  CHECK(limit.opacity == 10);          // 5 clamps up to the floor
  CHECK(limit.size == MOTO_UI_SIZE_M);
}

void test_apply_layout_is_reversible_and_idempotent() {
  moto_nav_ui_set_page(MOTO_UI_PAGE_NAVIGATION);
  moto_ui_state_t state = base_state();
  fill_route(state, 12);
  fill_streets(state);

  // A layout with the map hidden and only the speed component enabled.
  moto_ui_layout_t layout{};
  layout.north_up = 0;                 // heading-up
  layout.music_page_enabled = 1;
  layout.map_visible = 0;
  layout.theme_id = 0;
  layout.map_height_px = 300;
  layout.map_opacity = 40;
  layout.route_r = 0x12; layout.route_g = 0x34; layout.route_b = 0x56;
  layout.route_width_px = 9;
  layout.route_opacity = 55;
  layout.street_r = 0x11; layout.street_g = 0x22; layout.street_b = 0x33;
  layout.street_width_px = 5;
  layout.street_opacity = 20;
  layout.building_opacity = 60;
  layout.layer_mask = 0x05;            // roads + route, no buildings
  layout.zoom_percent = 150;
  layout.component_count = 2;
  layout.components[0] = {MOTO_UI_COMPONENT_SPEED, 1, MOTO_UI_SIZE_L, 100,
                          100, 100, 0, 0};
  layout.components[1] = {MOTO_UI_COMPONENT_SPEED_ARC, 1, MOTO_UI_SIZE_S, 50,
                          0, 0, MOTO_UI_ARC_LEFT, 180};
  moto_nav_ui_apply_layout(&layout);
  moto_nav_ui_set_state(&state);
  const Frame custom = capture();

  // Applying the very same layout again must not move a single pixel.
  moto_nav_ui_apply_layout(&layout);
  moto_nav_ui_set_state(&state);
  const Frame repeated = capture();
  CHECK(custom.hash == repeated.hash);

  // A different layout must visibly change the frame (the map is hidden here).
  moto_ui_layout_t restored = layout;
  restored.map_visible = 1;
  restored.map_height_px = MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT;
  moto_nav_ui_apply_layout(&restored);
  moto_nav_ui_set_state(&state);
  const Frame with_map = capture();
  CHECK(with_map.hash != custom.hash);

  // Heading-up keeps the marker unrotated; north-up rotates it, so the two
  // orientations must render differently with the same geometry.
  moto_ui_layout_t north_up = restored;
  north_up.north_up = 1;
  moto_nav_ui_apply_layout(&north_up);
  moto_nav_ui_set_state(&state);
  const Frame north_frame = capture();
  moto_ui_layout_t heading_up = restored;
  heading_up.north_up = 0;
  moto_nav_ui_apply_layout(&heading_up);
  moto_nav_ui_set_state(&state);
  const Frame heading_frame = capture();
  CHECK(north_frame.hash != heading_frame.hash);

  // A null layout must be ignored, and an all-zero layout (zero components,
  // zero map height) must clamp instead of corrupting the view.
  moto_nav_ui_apply_layout(nullptr);
  const moto_ui_layout_t* unchanged = moto_nav_ui_current_layout();
  CHECK(unchanged != nullptr);
  if (unchanged != nullptr) {
    CHECK(unchanged->north_up == heading_up.north_up);
  }
  moto_ui_layout_t fallback{};
  fallback.component_count = MOTO_UI_LAYOUT_COMPONENT_COUNT;
  moto_nav_ui_apply_layout(&fallback);
  moto_nav_ui_set_state(&state);
  const Frame fallback_frame = capture();
  const moto_ui_layout_t* current = moto_nav_ui_current_layout();
  CHECK(current != nullptr);
  if (current != nullptr) {
    // A zero height clamps to the documented minimum, never to zero.
    CHECK(current->map_height_px == MOTO_UI_LAYOUT_MIN_MAP_HEIGHT);
    // The array is sized but unpopulated, so no component is addressable.
    CHECK(current->component_count == MOTO_UI_LAYOUT_COMPONENT_COUNT);
    for (std::uint8_t i = 0; i < current->component_count; ++i) {
      CHECK(current->components[i].id == 0);
    }
  }
  // Re-applying the same fallback must be idempotent too.
  moto_nav_ui_apply_layout(&fallback);
  moto_nav_ui_set_state(&state);
  CHECK(capture().hash == fallback_frame.hash);

  // Leave the music page enabled again: the effective switch is the AND of the
  // layout flag and the media capability, and the following tests page to it.
  moto_ui_layout_t music_on = fallback;
  music_on.music_page_enabled = 1;
  moto_nav_ui_apply_layout(&music_on);
}

void test_component_visibility_is_layout_and_data_gated() {
  moto_nav_ui_set_page(MOTO_UI_PAGE_NAVIGATION);
  // Start from the full default layout so every component is addressable.
  moto_ui_layout_t full = *moto_nav_ui_current_layout();
  full.component_count = MOTO_UI_LAYOUT_COMPONENT_COUNT;
  full.components[MOTO_UI_COMPONENT_SPEED - 1] = {
      MOTO_UI_COMPONENT_SPEED, 1, MOTO_UI_SIZE_M, 100, 185, 300, 0, 0};
  full.components[MOTO_UI_COMPONENT_SPEED_LIMIT - 1] = {
      MOTO_UI_COMPONENT_SPEED_LIMIT, 1, MOTO_UI_SIZE_M, 100, 300, 238, 0, 0};
  full.components[MOTO_UI_COMPONENT_HEADING - 1] = {
      MOTO_UI_COMPONENT_HEADING, 0, MOTO_UI_SIZE_M, 100, 24, 40, 0, 0};
  full.components[MOTO_UI_COMPONENT_MANEUVER - 1] = {
      MOTO_UI_COMPONENT_MANEUVER, 1, MOTO_UI_SIZE_M, 100, 29, 220, 0, 0};
  full.components[MOTO_UI_COMPONENT_REMAINING_DISTANCE - 1] = {
      MOTO_UI_COMPONENT_REMAINING_DISTANCE, 1, MOTO_UI_SIZE_M, 90, 150, 390, 0, 0};
  full.components[MOTO_UI_COMPONENT_REMAINING_TIME - 1] = {
      MOTO_UI_COMPONENT_REMAINING_TIME, 1, MOTO_UI_SIZE_M, 90, 250, 390, 0, 0};
  full.components[MOTO_UI_COMPONENT_PROGRESS_ARC - 1] = {
      MOTO_UI_COMPONENT_PROGRESS_ARC, 1, MOTO_UI_SIZE_M, 80, 0, 0,
      MOTO_UI_ARC_TOP, 120};
  full.components[MOTO_UI_COMPONENT_SPEED_ARC - 1] = {
      MOTO_UI_COMPONENT_SPEED_ARC, 1, MOTO_UI_SIZE_M, 80, 0, 0,
      MOTO_UI_ARC_BOTTOM, 120};
  moto_nav_ui_apply_layout(&full);

  // Guidance present: maneuver / remaining distance / time / progress arc are
  // data-available; speed and the speed arc never depend on a route.
  moto_ui_state_t guided = base_state();
  fill_route(guided, 12);
  guided.speed_limit_kph = 50;
  moto_nav_ui_set_state(&guided);
  const Frame with_guidance = capture();

  // No guidance at all: the guidance-only components must disappear.
  moto_ui_state_t idle = guided;
  idle.route_point_count = 0;
  idle.speed_limit_kph = 0;
  idle.mode = MOTO_UI_ACQUIRING_FIX;
  moto_nav_ui_set_state(&idle);
  const Frame without_guidance = capture();
  CHECK(with_guidance.hash != without_guidance.hash);

  // Restoring guidance must restore the exact previous frame: no residue and
  // nothing accumulated while the components were hidden.
  moto_nav_ui_set_state(&guided);
  const Frame guidance_again = capture();
  CHECK(guidance_again.hash == with_guidance.hash);

  // A layout with everything disabled must hide the speed readout too, even
  // though speed data is always available.
  moto_ui_layout_t layout = *moto_nav_ui_current_layout();
  layout.component_count = 0;
  moto_nav_ui_apply_layout(&layout);
  moto_nav_ui_set_state(&guided);
  const Frame all_off = capture();
  CHECK(all_off.hash != with_guidance.hash);

  // Restore the documented default layout so later tests keep the 232 px map
  // viewport they were written against.
  moto_ui_layout_t defaults{};
  defaults.north_up = 1;
  defaults.music_page_enabled = 1;
  defaults.map_visible = 1;
  defaults.theme_id = 0;
  defaults.map_height_px = MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT;
  defaults.map_opacity = 100;
  defaults.route_r = 0xFF; defaults.route_g = 0xFF; defaults.route_b = 0xFF;
  defaults.route_width_px = 6;
  defaults.route_opacity = 100;
  defaults.street_r = 0x8E; defaults.street_g = 0x8E; defaults.street_b = 0x93;
  defaults.street_width_px = 3;
  defaults.street_opacity = 35;
  defaults.building_opacity = 25;
  defaults.layer_mask = 0x07;
  defaults.zoom_percent = 100;
  defaults.component_count = MOTO_UI_LAYOUT_COMPONENT_COUNT;
  moto_nav_ui_apply_layout(&defaults);
  moto_nav_ui_set_state(&guided);
  capture();
}

void test_music_page_state_and_fallback() {
  moto_nav_ui_set_music_page_enabled(1);
  moto_nav_ui_set_page(MOTO_UI_PAGE_MUSIC);
  CHECK(moto_nav_ui_get_page() == MOTO_UI_PAGE_MUSIC);

  moto_music_state_t music{};
  music.connected = 1;
  music.playing = 1;
  music.like_available = 1;
  music.source_name = "NetEase";
  music.track_title = "Road Song";
  music.artist_name = "New Pants";
  moto_nav_ui_set_music_state(&music);
  const Frame titled = capture();

  // Emptying the strings must visibly clear the labels (no stale text).
  moto_music_state_t empty_state{};
  empty_state.connected = 1;
  moto_nav_ui_set_music_state(&empty_state);
  const Frame emptied = capture();
  CHECK(titled.hash != emptied.hash);

  // Disabling the music page while it is on screen must fall back to the
  // navigation page; re-enabling must not restore it by itself.
  moto_nav_ui_set_music_page_enabled(0);
  pump();
  CHECK(moto_nav_ui_get_page() != MOTO_UI_PAGE_MUSIC);
}

void test_phone_lifecycle_renders_deterministically() {
  // The connection lifecycle is a full-screen overlay for the empty
  // navigation page; cycle it there and require visually distinct terminal
  // states plus deterministic repetition (no accumulated residue).
  moto_ui_state_t empty = base_state();
  empty.mode = MOTO_UI_ACQUIRING_FIX;
  empty.has_destination = 0;
  empty.route_point_count = 0;
  empty.road_name = nullptr;
  empty.next_road_name = nullptr;
  moto_nav_ui_set_state(&empty);
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_OFFLINE);
  const Frame offline = capture();
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_ONLINE);
  const Frame online = capture();
  CHECK(offline.hash != online.hash);

  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_OFFLINE);
  const Frame offline_again = capture();
  CHECK(offline_again.hash == offline.hash);

  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_CONNECTING);
  const Frame connecting = capture();
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_OFFLINE); CHECK(capture().hash == offline.hash);
}

void test_demo_mode_visual_contract() {
  // Demo mode suppresses the full-screen connection lifecycle and labels the
  // status line "DEMO RIDE"; toggling it must be perfectly reversible.
  moto_ui_state_t empty = base_state();
  empty.mode = MOTO_UI_ACQUIRING_FIX;
  empty.has_destination = 0;
  empty.route_point_count = 0;
  empty.road_name = nullptr;
  empty.next_road_name = nullptr;
  moto_nav_ui_set_state(&empty);
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_OFFLINE);
  moto_nav_ui_set_demo_active(0);
  const Frame without_demo = capture();

  moto_nav_ui_set_demo_active(1);
  const Frame with_demo = capture();
  CHECK(with_demo.hash != without_demo.hash);

  moto_nav_ui_set_demo_active(0);
  CHECK(capture().hash == without_demo.hash);

  // Contract: demo mode is connection-independent. The lifecycle surface is
  // hidden while demo is active, so connecting the phone must not change a
  // single pixel of the demo frame.
  moto_nav_ui_set_demo_active(1);
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_ONLINE);
  CHECK(capture().hash == with_demo.hash);
  moto_nav_ui_set_demo_active(0);
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_OFFLINE);
}

void test_capacity_payloads_render_stably() {
  // Maximum-capacity geometry must render identically across repeats.
  moto_ui_state_t max_state = base_state();
  fill_route(max_state, -1);
  fill_streets(max_state);
  max_state.road_point_count = MOTO_UI_ROAD_POINT_CAPACITY;
  for (int i = 0; i < MOTO_UI_ROAD_POINT_CAPACITY; ++i) {
    max_state.road_points[i] = {
        static_cast<std::int16_t>((i * 7) % kWidth),
        static_cast<std::int16_t>((i * 13) % kHeight)};
  }
  max_state.road_polyline_count = 4;
  for (int i = 0; i < 4; ++i) {
    max_state.road_polylines[i] = {
        static_cast<std::uint8_t>(i * 48),
        48,
        static_cast<std::uint8_t>(i % 6)};
  }
  moto_nav_ui_set_state(&max_state);
  const Frame max_first = capture();
  moto_nav_ui_set_state(&max_state);
  const Frame max_second = capture();
  CHECK(max_first.hash == max_second.hash);
}

void test_motion_frames_interpolate_then_settle_without_residue() {
  moto_nav_ui_set_reduce_motion(1);
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_ONLINE);
  moto_ui_state_t state = base_state();
  state.route_point_count = 3;
  state.route_points[0] = {233, 254};
  state.route_points[1] = {233, 150};
  state.route_points[2] = {233, 30};
  state.road_point_count = 2;
  state.road_points[0] = {110, 280};
  state.road_points[1] = {110, 30};
  state.road_polyline_count = 1;
  state.road_polylines[0] = {0, 2, 1};
  state.building_point_count = 4;
  state.building_points[0] = {300, 80};
  state.building_points[1] = {350, 80};
  state.building_points[2] = {350, 130};
  state.building_points[3] = {300, 130};
  state.building_footprint_count = 1;
  state.building_footprints[0] = {0, 4, 1};
  moto_nav_ui_set_state(&state);
  pump(260);  // finish the transient connected indication before comparing
  const auto before = capture().hash;

  // One heading frame moves all retained arrays through the ordinary motion
  // path, preserving the anchor and topology while the map turns under it.
  for (int index = 1; index < 3; ++index) state.route_points[index].x -= 130;
  for (int index = 0; index < 2; ++index) state.road_points[index].x += 110;
  for (int index = 0; index < 4; ++index) state.building_points[index].x -= 90;
  moto_nav_ui_set_reduce_motion(0);
  moto_nav_ui_set_motion_state(&state);
  pump(10);  // 50 ms contains two 25 ms interpolation opportunities.
  const auto intermediate = frame_hash();
  CHECK(intermediate != before);
  pump(200);
  const auto settled = frame_hash();
  const auto settled_bytes = shadow_frame;
  CHECK(settled != intermediate);

  // The eventual animated frame must match a direct render exactly, including
  // clearing the old road/building pixels in the shared invalidated region.
  moto_nav_ui_set_reduce_motion(1);
  moto_nav_ui_set_state(&state);
  const auto direct = capture();
  // Compare the map viewport. The page dots below it expire independently at
  // five seconds, which may fall between the two captured frames.
  constexpr int map_height = MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT;
  constexpr std::size_t map_bytes = kWidth * map_height * (LV_COLOR_DEPTH / 8);
  CHECK(std::equal(direct.bytes.begin(), direct.bytes.begin() + map_bytes,
                   settled_bytes.begin()));
}

void test_diagonal_map_pixels_are_independent_of_partial_buffer_height() {
  moto_nav_ui_set_reduce_motion(1);
  moto_nav_ui_set_phone_connection(MOTO_UI_PHONE_ONLINE);
  auto state = base_state();
  fill_route(state, -1);
  fill_streets(state);
  state.road_point_count = MOTO_UI_ROAD_POINT_CAPACITY;
  state.road_polyline_count = MOTO_UI_ROAD_POLYLINE_CAPACITY;
  for (int i = 0; i < state.road_point_count; ++i) {
    // Long crossing diagonals, partially outside the viewport, exercise
    // band borders, both slope signs and the different road stroke widths.
    state.road_points[i] = {
        static_cast<std::int16_t>((i * 83) % 780 - 160),
        static_cast<std::int16_t>((i * 127) % 590 - 110)};
  }
  for (int i = 0; i < state.road_polyline_count; ++i) {
    state.road_polylines[i] = {
        static_cast<std::uint8_t>(i * 8), 8,
        static_cast<std::uint8_t>(i % 6)};
  }
  moto_nav_ui_set_state(&state);
  pump(260);
  auto* display = lv_display_get_default();
  static std::vector<std::uint8_t> short_buffer(kWidth * 40 * 2);
  static std::vector<std::uint8_t> tall_buffer(kWidth * 320 * 2);
  const auto draw_with_buffer = [display](std::vector<std::uint8_t>& buffer) {
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size(),
                            LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    return shadow_frame;
  };
  const auto short_frame = draw_with_buffer(short_buffer);
  const auto tall_frame = draw_with_buffer(tall_buffer);
  CHECK(tall_frame == short_frame);
  CHECK(draw_with_buffer(short_buffer) == tall_frame);
}

}  // namespace

int main(int argc, char **argv) {
  const int buffer_rows = argc > 1 ? std::atoi(argv[1]) : 40;
  const int extra_pool_bytes = argc > 2 ? std::atoi(argv[2]) : 0;
  if(buffer_rows < 1 || buffer_rows > kHeight) return EXIT_FAILURE;
  if(extra_pool_bytes < 0) return EXIT_FAILURE;
  init_display(buffer_rows, extra_pool_bytes);

  // Production cadence: the boot animation completes before the full UI is
  // created, so settle it before moto_nav_ui_create().
  moto_nav_ui_show_boot_screen();
  pump(260);
  const Frame settled_boot = capture();
  const bool all_black = std::all_of(
      settled_boot.bytes.begin(), settled_boot.bytes.end(),
      [](std::uint8_t byte) { return byte == 0x00; });
  CHECK(all_black);

  test_frames_are_deterministic();
  test_clearing_geometry_leaves_no_residue();
  test_layout_defaults_match_the_documented_baseline();
  test_apply_layout_clamps_out_of_range_values();
  test_apply_layout_is_reversible_and_idempotent();
  test_component_visibility_is_layout_and_data_gated();
  test_music_page_state_and_fallback();
  test_phone_lifecycle_renders_deterministically();
  test_demo_mode_visual_contract();
  test_capacity_payloads_render_stably();
  test_motion_frames_interpolate_then_settle_without_residue();
  test_diagonal_map_pixels_are_independent_of_partial_buffer_height();

  // Terminal screens run last: they tear the full UI down.
  moto_nav_ui_show_boot_screen();
  pump(260);
  const Frame boot_again = capture();
  CHECK(std::all_of(boot_again.bytes.begin(), boot_again.bytes.end(),
                    [](std::uint8_t byte) { return byte == 0x00; }));

  moto_nav_ui_show_power_off_screen();
  pump(260);
  const Frame power_off = capture();
  moto_nav_ui_show_power_off_screen();
  pump(260);
  CHECK(power_off.hash == capture().hash);
  CHECK(power_off.hash != settled_boot.hash);

  if (failures != 0) {
    std::cerr << failures << " nav_ui render checks failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All nav_ui render checks passed\n";
  return EXIT_SUCCESS;
}
