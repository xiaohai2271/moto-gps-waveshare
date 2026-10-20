#include "moto_nav_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "lvgl.h"


#include "accel_filter.h"
#include "layout_geometry.h"

// The generated font sources are plain C translation units, so their public
// descriptors carry C linkage. Declaring them with extern "C" here keeps the
// MSVC host build (which compiles .c as C and this file as C++) linkable;
// LV_FONT_DECLARE alone would mangle the symbol under /std:c++17.
extern "C" {
extern const lv_font_t moto_font_nav_16;
extern const lv_font_t moto_font_nav_20;
extern const lv_font_t moto_font_nav_28;
}

// Platform-neutral trace hook: the shared build (web/iOS/host tests) leaves it
// empty; the ESP32 build overrides it in platforms/esp32/main to reach ESP_LOG.
// GCC/Clang (ESP32, web, iOS) use a weak symbol; MSVC has no such spelling, so
// it just gets a strong empty definition there.
#if defined(__GNUC__) || defined(__clang__)
#define MOTO_NAV_UI_TRACE_WEAK __attribute__((weak))
#else
#define MOTO_NAV_UI_TRACE_WEAK
#endif

extern "C" MOTO_NAV_UI_TRACE_WEAK void navui_trace(const char *message) {
    (void)message;
}

namespace {

constexpr lv_color_t kBlack = LV_COLOR_MAKE(0x05, 0x06, 0x07);
constexpr lv_color_t kWhite = LV_COLOR_MAKE(0xF3, 0xF4, 0xEF);
constexpr lv_color_t kIce = LV_COLOR_MAKE(0xB8, 0xED, 0xF5);
constexpr lv_color_t kGraphite = LV_COLOR_MAKE(0x30, 0x35, 0x39);
// Real surrounding roads need to remain legible on the AMOLED's true-black
// background.  LV_OPA_70 is 70/255 (not 70 percent), which made the previous
// road layer effectively disappear on the physical display.
constexpr lv_color_t kRoadGray = LV_COLOR_MAKE(0x42, 0x47, 0x4B);
constexpr lv_color_t kRoadMajor = LV_COLOR_MAKE(0x62, 0x69, 0x6D);
constexpr lv_color_t kRoadMinor = LV_COLOR_MAKE(0x2B, 0x30, 0x33);
constexpr lv_color_t kBuildingGray = LV_COLOR_MAKE(0x24, 0x29, 0x2C);
constexpr lv_color_t kBuildingLandmark = LV_COLOR_MAKE(0x38, 0x40, 0x44);
constexpr lv_color_t kQuiet = LV_COLOR_MAKE(0x78, 0x7E, 0x7F);
constexpr lv_color_t kSoft = LV_COLOR_MAKE(0xAE, 0xB2, 0xB0);
constexpr lv_color_t kAmber = LV_COLOR_MAKE(0xE6, 0xC8, 0x4F);
constexpr lv_color_t kRed = LV_COLOR_MAKE(0xFF, 0x4B, 0x43);
constexpr lv_color_t kGreen = LV_COLOR_MAKE(0x69, 0xD4, 0x94);
constexpr double kPi = 3.14159265358979323846;
constexpr int kCompassTickCount = 24;
constexpr int kSpeedTickCount = 18;
constexpr int kDesignWidth = 360;
constexpr std::uint32_t kPageDotsVisibleMs = 5'000;
// Keep LVGL, the UI interpolation timer and IMU presentation on the same
// 40 Hz cadence. The previous 40/33 ms mismatch periodically produced a
// 66 ms visual gap even when both tasks were otherwise keeping up.
constexpr std::uint32_t kRouteMotionFrameMs = 25;
static_assert(LV_DEF_REFR_PERIOD == kRouteMotionFrameMs,
              "LVGL refresh and map motion must use the same cadence");
constexpr std::uint32_t kConnectionSuccessHoldMs = 920;

enum class LifecycleVisual : std::uint8_t {
    Hidden = 0,
    PhoneOffline,
    PhoneConnecting,
    Connected,
    Ready,
    Planning,
};

constexpr int px(int value) {
    return value >= 0
               ? (value * MOTO_UI_CANVAS_WIDTH + kDesignWidth / 2) /
                     kDesignWidth
               : -((-value * MOTO_UI_CANVAS_WIDTH + kDesignWidth / 2) /
                   kDesignWidth);
}

constexpr double px(double value) {
    return value * static_cast<double>(MOTO_UI_CANVAS_WIDTH) /
           static_cast<double>(kDesignWidth);
}

struct MapPolyline {
    const lv_point_precise_t *points = nullptr;
    std::uint16_t count = 0;
};

// Forward declarations: the Ui struct keeps the live layout, and
// reset_ui_state() seeds it before the layout engine below is defined.
moto_ui_layout_t default_layout();
void apply_layout_internal(const moto_ui_layout_t *incoming);

struct Ui {
    lv_obj_t *screen = nullptr;
    lv_obj_t *pages[MOTO_UI_PAGE_COUNT]{};
    lv_obj_t *page_dots[MOTO_UI_PAGE_COUNT]{};
    lv_timer_t *page_dots_timer = nullptr;
    moto_ui_page_t page = MOTO_UI_PAGE_NAVIGATION;
    bool page_dots_visible = true;

    lv_obj_t *nav_map = nullptr;
    lv_obj_t *nav_buildings[MOTO_UI_BUILDING_FOOTPRINT_CAPACITY]{};
    MapPolyline nav_building_lines[MOTO_UI_BUILDING_FOOTPRINT_CAPACITY]{};
    lv_point_precise_t nav_building_points[
        MOTO_UI_BUILDING_POINT_CAPACITY +
        MOTO_UI_BUILDING_FOOTPRINT_CAPACITY]{};
    float nav_building_x[MOTO_UI_BUILDING_POINT_CAPACITY]{};
    float nav_building_y[MOTO_UI_BUILDING_POINT_CAPACITY]{};
    float nav_building_target_x[MOTO_UI_BUILDING_POINT_CAPACITY]{};
    float nav_building_target_y[MOTO_UI_BUILDING_POINT_CAPACITY]{};
    moto_ui_building_span_t
        nav_building_spans[MOTO_UI_BUILDING_FOOTPRINT_CAPACITY]{};
    std::uint8_t nav_building_point_count = 0;
    std::uint8_t nav_building_footprint_count = 0;
    std::uint32_t nav_building_scene_revision = 0;
    lv_obj_t *nav_roads[MOTO_UI_ROAD_POLYLINE_CAPACITY]{};
    MapPolyline nav_road_lines[MOTO_UI_ROAD_POLYLINE_CAPACITY]{};
    lv_point_precise_t nav_road_points[MOTO_UI_ROAD_POINT_CAPACITY]{};
    float nav_road_x[MOTO_UI_ROAD_POINT_CAPACITY]{};
    float nav_road_y[MOTO_UI_ROAD_POINT_CAPACITY]{};
    float nav_road_target_x[MOTO_UI_ROAD_POINT_CAPACITY]{};
    float nav_road_target_y[MOTO_UI_ROAD_POINT_CAPACITY]{};
    moto_ui_polyline_span_t
        nav_road_spans[MOTO_UI_ROAD_POLYLINE_CAPACITY]{};
    std::uint8_t nav_road_point_count = 0;
    std::uint8_t nav_road_polyline_count = 0;
    std::uint32_t nav_road_scene_revision = 0;
    lv_obj_t *nav_route_shadow = nullptr;
    lv_obj_t *nav_route = nullptr;
    MapPolyline nav_route_shadow_line{};
    MapPolyline nav_route_line{};
    lv_point_precise_t nav_route_points[MOTO_UI_ROUTE_POINT_CAPACITY]{};
    float nav_route_x[MOTO_UI_ROUTE_POINT_CAPACITY]{};
    float nav_route_y[MOTO_UI_ROUTE_POINT_CAPACITY]{};
    float nav_route_target_x[MOTO_UI_ROUTE_POINT_CAPACITY]{};
    float nav_route_target_y[MOTO_UI_ROUTE_POINT_CAPACITY]{};
    std::uint8_t nav_route_point_count = 0;
    std::uint8_t nav_route_target_count = 0;
    std::uint32_t nav_route_identity = 0;
    std::uint32_t nav_route_generation = 0;
    lv_timer_t *nav_route_motion_timer = nullptr;
    lv_obj_t *nav_marker = nullptr;    lv_obj_t *nav_status = nullptr;
    lv_obj_t *nav_distance = nullptr;
    lv_obj_t *nav_unit = nullptr;
    lv_obj_t *nav_maneuver = nullptr;
    moto_maneuver_t nav_maneuver_type = MOTO_MANEUVER_STRAIGHT;
    lv_obj_t *nav_limit = nullptr;
    lv_obj_t *nav_limit_value = nullptr;
    lv_obj_t *nav_progress = nullptr;

    // --- Editable layout components (plan §4.2) ---------------------------
    // Every one of these is created once and only repositioned/resized by
    // apply_layout(); per-frame updates only set text/values.
    lv_obj_t *nav_map_base = nullptr;      // roads + buildings layer
    lv_obj_t *nav_route_layer = nullptr;   // route + shadow layer
    lv_obj_t *nav_speed = nullptr;         // speed value + unit
    lv_obj_t *nav_speed_unit = nullptr;
    lv_obj_t *nav_heading = nullptr;       // heading angle + cardinal
    lv_obj_t *nav_heading_cardinal = nullptr;
    lv_obj_t *nav_remaining_distance = nullptr;
    lv_obj_t *nav_remaining_distance_caption = nullptr;
    lv_obj_t *nav_remaining_time = nullptr;
    lv_obj_t *nav_remaining_time_caption = nullptr;
    lv_obj_t *nav_accel = nullptr;         // signed acceleration readout
    lv_obj_t *nav_speed_arc_object = nullptr;  // speed/acceleration arc
    // The maneuver glyph is authored for the M bounding box; S and L scale the
    // same artwork (§4.3) so no second icon set is needed.
    float maneuver_scale = 1.0F;
    // The layout currently applied. Defaults match Android LayoutDefaults
    // (§4.10) so an unpushed device shows the same page as the editor.
    moto_ui_layout_t layout{};
    bool layout_applied = false;
    // Media capability gate for the music page (bridge-owned).
    bool media_music_available = true;
    // Acceleration smoothing (§6.4); pure logic lives in accel_filter.h.
    moto::nav::ui::AccelFilter accel_filter{};
    // Acceleration readout repaint throttle (deterministic renders).
    std::uint32_t accel_readout_ms = 0;
    bool accel_readout_valid = false;
    unsigned accel_readout_speed_kph = 0xFFFFFFFFu;

    // A full-screen empty-state surface replaces the old tiny
    // WAITING FOR PHONE / WAITING FOR ROUTE caption. It is kept inside the
    // navigation page so the speed and compass tools remain independently
    // usable while the phone is disconnected.
    lv_obj_t *nav_lifecycle = nullptr;
    lv_obj_t *nav_lifecycle_symbol = nullptr;
    lv_obj_t *nav_lifecycle_title = nullptr;
    lv_obj_t *nav_lifecycle_subtitle = nullptr;
    lv_obj_t *nav_lifecycle_kicker = nullptr;
    lv_timer_t *nav_lifecycle_timer = nullptr;
    lv_timer_t *nav_success_timer = nullptr;
    moto_ui_phone_connection_t phone_connection = MOTO_UI_PHONE_OFFLINE;
    LifecycleVisual lifecycle_visual = LifecycleVisual::Hidden;
    LifecycleVisual lifecycle_target = LifecycleVisual::Hidden;
    std::uint16_t lifecycle_phase_deg = 0;
    bool lifecycle_success_active = false;
    bool navigation_has_guidance = false;
    bool navigation_route_request_in_flight = false;

    lv_obj_t *speed_arc = nullptr;
    lv_obj_t *speed_value = nullptr;
    lv_obj_t *speed_ticks[kSpeedTickCount]{};
    lv_point_precise_t speed_tick_points[kSpeedTickCount][2]{};

    lv_obj_t *compass_heading = nullptr;
    lv_obj_t *compass_cardinal = nullptr;
    lv_obj_t *compass_speed = nullptr;
    lv_obj_t *compass_ticks[kCompassTickCount]{};
    lv_point_precise_t compass_tick_points[kCompassTickCount][2]{};
    lv_obj_t *compass_letters[4]{};

    lv_obj_t *music_source = nullptr;
    lv_obj_t *music_title = nullptr;
    lv_obj_t *music_artist = nullptr;
    lv_obj_t *music_disc = nullptr;
    lv_obj_t *music_buttons[4]{};
    lv_obj_t *music_button_labels[4]{};
    moto_music_state_t music{};
    char music_source_text[32]{};
    char music_title_text[64]{};
    char music_artist_text[48]{};

    moto_music_command_callback_t music_callback = nullptr;
    void *music_callback_context = nullptr;
    moto_page_change_callback_t page_callback = nullptr;
    void *page_callback_context = nullptr;
    moto_demo_change_callback_t demo_callback = nullptr;
    void *demo_callback_context = nullptr;
    bool music_page_enabled = true;
    bool demo_active = false;
    bool reduce_motion = false;
} ui;

void reset_ui_state() {
    static_assert(std::is_trivially_copyable_v<Ui>,
                  "Ui must remain safe to reset without a stack temporary");

    // `ui = {}` materializes the complete aggregate as a temporary.  The
    // offline map buffers make Ui about 15 KiB, so that temporary can consume
    // nearly the whole ESP-IDF main-task stack before LVGL is called.  Clear
    // the retained instance in place and restore the two non-zero defaults.
    std::memset(&ui, 0, sizeof(ui));
    ui.page = MOTO_UI_PAGE_NAVIGATION;
    ui.page_dots_visible = true;
    ui.nav_maneuver_type = MOTO_MANEUVER_STRAIGHT;
    ui.phone_connection = MOTO_UI_PHONE_OFFLINE;
    ui.lifecycle_visual = LifecycleVisual::Hidden;
    ui.lifecycle_target = LifecycleVisual::Hidden;
    ui.music_page_enabled = true;
    ui.media_music_available = true;
    ui.layout = default_layout();
    ui.layout_applied = false;
    // Force the first geometry payload to bind every mutable LVGL line even
    // when its protocol revision happens to start at zero.
    ui.nav_route_generation = ~std::uint32_t{0};
    ui.nav_route_identity = ~std::uint32_t{0};
    ui.nav_road_scene_revision = ~std::uint32_t{0};
    ui.nav_building_scene_revision = ~std::uint32_t{0};
}

// ---------------------------------------------------------------------------
// Editable navigation-page layout engine (plan §4 / §6.2).
//
// Layout application is idempotent and cheap: it only repositions, resizes
// and restyles already-created objects. Nothing here allocates or recreates
// LVGL objects, so a push from the phone costs one frame at most.
// ---------------------------------------------------------------------------

/** §4.10 default layout; mirrors Android LayoutDefaults byte for byte. */
moto_ui_layout_t default_layout() {
    moto_ui_layout_t layout{};    layout.north_up = 1;
    layout.music_page_enabled = 1;
    layout.map_visible = 1;
    layout.theme_id = 0;
    layout.map_height_px = MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT;
    layout.map_opacity = 100;
    layout.route_r = 0xFF; layout.route_g = 0xFF; layout.route_b = 0xFF;
    layout.route_width_px = 6;
    layout.route_opacity = 100;
    layout.street_r = 0x8E; layout.street_g = 0x8E; layout.street_b = 0x93;
    layout.street_width_px = 3;
    layout.street_opacity = 35;
    layout.building_opacity = 25;
    layout.layer_mask = 0x07;
    layout.zoom_percent = 100;
    layout.component_count = 8;

    layout.components[0] = {MOTO_UI_COMPONENT_SPEED, 1, MOTO_UI_SIZE_M, 100, 185, 300, 0, 0};
    layout.components[1] = {MOTO_UI_COMPONENT_SPEED_LIMIT, 1, MOTO_UI_SIZE_M, 100, 300, 238, 0, 0};
    layout.components[2] = {MOTO_UI_COMPONENT_HEADING, 0, MOTO_UI_SIZE_M, 100, 24, 40, 0, 0};
    layout.components[3] = {MOTO_UI_COMPONENT_MANEUVER, 1, MOTO_UI_SIZE_M, 100, 29, 220, 0, 0};
    layout.components[4] = {MOTO_UI_COMPONENT_REMAINING_DISTANCE, 1, MOTO_UI_SIZE_M, 90, 150, 390, 0, 0};
    layout.components[5] = {MOTO_UI_COMPONENT_REMAINING_TIME, 1, MOTO_UI_SIZE_M, 90, 250, 390, 0, 0};
    layout.components[6] = {MOTO_UI_COMPONENT_PROGRESS_ARC, 1, MOTO_UI_SIZE_M, 80, 0, 0, MOTO_UI_ARC_TOP, 120};
    layout.components[7] = {MOTO_UI_COMPONENT_SPEED_ARC, 1, MOTO_UI_SIZE_M, 80, 0, 0, MOTO_UI_ARC_BOTTOM, 120};
    return layout;
}

const moto_ui_component_layout_t *find_component(const moto_ui_layout_t &layout,
                                                 std::uint8_t id) {
    for(std::uint8_t i = 0; i < layout.component_count; ++i) {
        if(layout.components[i].id == id) return &layout.components[i];
    }
    return nullptr;
}

/** §4.3 size table: (S, M, L) outer rectangles in 466-canvas design px. */
void component_box(std::uint8_t id, std::uint8_t size, int &width, int &height) {
    int s = 0, m = 0, l = 0;
    switch(id) {
        case MOTO_UI_COMPONENT_SPEED:               s = 72; m = 96; l = 124; break;
        case MOTO_UI_COMPONENT_SPEED_LIMIT:         s = 44; m = 61; l = 78; break;
        case MOTO_UI_COMPONENT_HEADING:             s = 68; m = 88; l = 112; break;
        case MOTO_UI_COMPONENT_MANEUVER:            s = 150; m = 180; l = 210; break;
        case MOTO_UI_COMPONENT_REMAINING_DISTANCE:  s = 92; m = 116; l = 146; break;
        case MOTO_UI_COMPONENT_REMAINING_TIME:      s = 78; m = 100; l = 128; break;
        default: break;
    }
    // Non-text components also carry a height axis; pairs are (w,h) triples.
    static const int kHeights[6][3] = {
        {56, 72, 92}, {44, 61, 78}, {60, 76, 96},
        {108, 128, 148}, {44, 56, 68}, {44, 56, 68},
    };
    const int index = id - 1;
    width = (size == MOTO_UI_SIZE_S) ? s : (size == MOTO_UI_SIZE_L) ? l : m;
    height = (index >= 0 && index < 6)
                 ? ((size == MOTO_UI_SIZE_S) ? kHeights[index][0]
                    : (size == MOTO_UI_SIZE_L) ? kHeights[index][2]
                                               : kHeights[index][1])
                 : 0;
}

int arc_width_for_size(std::uint8_t size) {
    return size == MOTO_UI_SIZE_S ? 4 : (size == MOTO_UI_SIZE_L ? 8 : 6);
}

int opacity_opa(std::uint8_t opacity) {
    if(opacity > 100) opacity = 100;
    if(opacity < 10) opacity = 10;
    return opacity * 255 / 100;
}

int text_opacity_opa(std::uint8_t opacity) {
    // 0 is the wire default for "not specified": keep text fully visible.
    if(opacity == 0) opacity = 100;
    if(opacity > 100) opacity = 100;
    if(opacity < 10) opacity = 10;
    return opacity * 255 / 100;
}

/** LVGL arc centre angle for an anchor (0 deg = right, clockwise). */
int anchor_center_angle(std::uint8_t anchor) {
    switch(anchor) {
        case MOTO_UI_ARC_TOP: return 270;
        case MOTO_UI_ARC_RIGHT: return 0;
        case MOTO_UI_ARC_BOTTOM: return 90;
        case MOTO_UI_ARC_LEFT: return 180;
        default: return 270;
    }
}

std::uint16_t clamp_span(std::uint16_t span_deg) {
    return (span_deg == 90 || span_deg == 180) ? span_deg : 120;
}

void place_component(lv_obj_t *object, const moto_ui_component_layout_t &c) {
    int width = 0, height = 0;
    component_box(c.id, c.size, width, height);
    // Layout coordinates and the §4.3 size table are already expressed in the
    // 466 px canvas space (see component_box and docs §4.2/§4.3). They must not
    // go through px(), which rescales the legacy 360 px design space: doing so
    // pushed y=390 to 505 (off-screen) and doubled the text boxes.
    if(width > 0) lv_obj_set_size(object, width, height);
    lv_obj_set_pos(object, c.x, c.y);
}

// Arc ids are the only components whose geometry is decided by the arc rules
// rather than the rectangular size table.
bool layout_component_is_arc(std::uint8_t id) {
    return id == MOTO_UI_COMPONENT_PROGRESS_ARC ||
           id == MOTO_UI_COMPONENT_SPEED_ARC;
}

void apply_arc(lv_obj_t *arc, const moto_ui_component_layout_t &c, int diameter,
               std::uint8_t opacity) {
    // 440/408 are 466-canvas diameters (§4.3); they are used verbatim so the
    // arc radius matches the editor preview instead of growing 1.29x.
    lv_obj_set_size(arc, diameter, diameter);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    const int center = anchor_center_angle(c.anchor);
    const int span = clamp_span(c.span_deg);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, center - span / 2, center + span / 2);
    lv_obj_set_style_arc_width(arc, arc_width_for_size(c.size), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, arc_width_for_size(c.size),
                               LV_PART_INDICATOR);
    const int opa = opacity_opa(opacity);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_INDICATOR);
}

// §4.3: a size tier changes the object box AND the text font together, so an S
// component stays legible instead of shrinking the box around an L glyph.
// Only the fonts already linked into the build are used (no new font assets):
// Montserrat 16/28/48 for values and Montserrat 20 for captions.
const lv_font_t *font_for_tier(int size) {
    return size == MOTO_UI_SIZE_S   ? &lv_font_montserrat_16
           : size == MOTO_UI_SIZE_L ? &lv_font_montserrat_48
                                    : &lv_font_montserrat_28;
}

const lv_font_t *caption_font_for_tier(int size) {
    // Captions are UI copy ("剩余", "剩余时间", "北"), not numbers, so they need
    // the Chinese-capable generated fonts. Using Montserrat here rendered the
    // captions as tofu boxes on the device.
    return size == MOTO_UI_SIZE_S ? &moto_font_nav_16 : &moto_font_nav_20;
}

void apply_component(lv_obj_t *object, const moto_ui_component_layout_t &c,
                     lv_obj_t *secondary = nullptr) {
    if(object == nullptr) return;
    if(!c.enabled) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        if(secondary != nullptr) lv_obj_add_flag(secondary, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    if(secondary != nullptr) lv_obj_clear_flag(secondary, LV_OBJ_FLAG_HIDDEN);
    if(layout_component_is_arc(c.id)) {
        // Arcs are sized/anchored by their dedicated helper.
        return;
    }
    place_component(object, c);
    lv_obj_set_style_text_font(object, font_for_tier(c.size), 0);
    if(secondary != nullptr) {
        lv_obj_set_style_text_font(secondary, caption_font_for_tier(c.size), 0);
    }
    lv_obj_set_style_opa(object, opacity_opa(c.opacity), 0);
    if(secondary != nullptr) {
        lv_obj_set_style_opa(secondary, opacity_opa(c.opacity), 0);
    }
}

// Text components are a two-line block: a centred value with its unit or
// caption directly underneath. The block is centred inside the §4.3 box so the
// value never drifts to the box corner, and the unit can no longer collide with
// the neighbouring speed-limit sign (device-reported overlap).
void place_text_component(lv_obj_t *object,
                          const moto_ui_component_layout_t &c,
                          lv_obj_t *secondary) {
    if(object == nullptr) return;
    if(!c.enabled) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        if(secondary != nullptr) lv_obj_add_flag(secondary, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    if(secondary != nullptr) lv_obj_clear_flag(secondary, LV_OBJ_FLAG_HIDDEN);
    int width = 0, height = 0;
    component_box(c.id, c.size, width, height);
    const lv_font_t *font = font_for_tier(c.size);
    const lv_font_t *caption = caption_font_for_tier(c.size);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(object, text_opacity_opa(c.text_opacity), 0);
    lv_obj_set_style_opa(object, LV_OPA_COVER, 0);
    const int line = lv_font_get_line_height(font);
    int total = line;
    if(secondary != nullptr) {
        lv_obj_set_style_text_font(secondary, caption, 0);
        lv_obj_set_style_text_align(secondary, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(secondary, text_opacity_opa(c.text_opacity), 0);
        lv_obj_set_style_opa(secondary, LV_OPA_COVER, 0);
        total += lv_font_get_line_height(caption) + 2;
    }
    lv_obj_set_size(object, width, line);
    lv_obj_set_pos(object, c.x, c.y + (height - total) / 2);
    if(secondary != nullptr) {
        lv_obj_align_to(secondary, object, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
}

lv_color_t rgb_color(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return LV_COLOR_MAKE(r, g, b);
}

lv_obj_t *make_layer(lv_obj_t *parent) {
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, MOTO_UI_CANVAS_WIDTH, MOTO_UI_CANVAS_HEIGHT);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    return layer;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                     lv_color_t color, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    return label;
}

void copy_text(char *destination, std::size_t capacity, const char *source,
               const char *fallback) {
    if(capacity == 0) return;
    const char *value = source != nullptr && source[0] != '\0' ? source : fallback;
    std::snprintf(destination, capacity, "%s", value != nullptr ? value : "");
}

lv_color_t traffic_color(moto_traffic_t traffic) {
    switch(traffic) {
        case MOTO_TRAFFIC_CLEAR: return kIce;
        case MOTO_TRAFFIC_SLOW: return kAmber;
        case MOTO_TRAFFIC_CONGESTED:
        case MOTO_TRAFFIC_SEVERE: return kRed;
        default: return kQuiet;
    }
}

const char *cardinal_name(std::uint16_t heading) {
    static const char *names[] = {"北", "东北", "东", "东南",
                                  "南", "西南", "西", "西北"};
    return names[((heading + 22U) / 45U) % 8U];
}

// Current vehicle-marker rotation in degrees, set by update_navigation() and
// consumed by draw_vehicle_marker() (file-scope because the draw callback has
// no access to the ui struct which is declared further down).
int g_marker_rotation_deg = 0;

void draw_vehicle_marker(lv_event_t *event) {
    lv_obj_t *object = lv_event_get_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    lv_layer_t *layer = lv_event_get_layer(event);

    // Rotate the arrow around the object centre in the draw callback instead
    // of using style transform_rotation: on this LVGL build a transformed
    // child of the map canvas rotated the whole page (device-verified bug).
    // Angles are clockwise from "pointing up", matching the compass heading.
    const double heading_rad =
        static_cast<double>(g_marker_rotation_deg) * kPi / 180.0;
    const double cos_a = std::cos(heading_rad);
    const double sin_a = std::sin(heading_rad);
    const double centre_x = static_cast<double>(area.x1 + area.x2 + 1) / 2.0;
    const double centre_y = static_cast<double>(area.y1 + area.y2 + 1) / 2.0;

    // Local arrow coordinates in px-design units, origin at centre, y up.
    // Outline triangle then the white inset, same shape as before.
    const double outline[3][2] = {{0.0, 18.5}, {-17.0, -18.0}, {17.0, -18.0}};
    const double inset[3][2] = {{0.0, 13.5}, {-11.5, -12.0}, {11.5, -12.0}};

    auto draw_arrow = [&](const double(&points)[3][2], lv_color_t color) {
        lv_draw_triangle_dsc_t triangle;
        lv_draw_triangle_dsc_init(&triangle);
        triangle.color = color;
        triangle.opa = LV_OPA_COVER;
        for(int i = 0; i < 3; ++i) {
            // Screen y grows downward: negate the local y.
            const double rx = points[i][0] * cos_a + points[i][1] * sin_a;
            const double ry = -points[i][0] * sin_a + points[i][1] * cos_a;
            triangle.p[i] = {static_cast<lv_value_precise_t>(centre_x + rx * px(1.0)),
                             static_cast<lv_value_precise_t>(centre_y - ry * px(1.0))};
        }
        lv_draw_triangle(layer, &triangle);
    };
    draw_arrow(outline, kBlack);
    draw_arrow(inset, kWhite);
}

void update_page_dots() {
    for(int i = 0; i < MOTO_UI_PAGE_COUNT; ++i) {
        const bool active = i == static_cast<int>(ui.page);
        lv_obj_set_size(ui.page_dots[i], px(active ? 16 : 5), px(5));
        lv_obj_set_style_radius(ui.page_dots[i], px(3), 0);
        lv_obj_set_style_bg_color(ui.page_dots[i], active ? kWhite : kGraphite, 0);
        lv_obj_set_x(ui.page_dots[i], px(145 + i * 22 - (active ? 5 : 0)));
        const bool available = i != MOTO_UI_PAGE_MUSIC || ui.music_page_enabled;
        if(ui.page_dots_visible && available) {
            lv_obj_remove_flag(ui.page_dots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.page_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void hide_page_dots(lv_timer_t *) {
    ui.page_dots_visible = false;
    update_page_dots();
    if(ui.page_dots_timer != nullptr) lv_timer_pause(ui.page_dots_timer);
}

void reveal_page_dots() {
    if(ui.screen == nullptr || ui.page_dots[0] == nullptr) return;
    ui.page_dots_visible = true;
    update_page_dots();
    if(ui.page_dots_timer != nullptr) {
        lv_timer_set_period(ui.page_dots_timer, kPageDotsVisibleMs);
        lv_timer_reset(ui.page_dots_timer);
        lv_timer_resume(ui.page_dots_timer);
    }
}

void interaction_event(lv_event_t *) {
    reveal_page_dots();
}

void install_interaction_wake(lv_obj_t *object) {
    if(object == nullptr) return;
    lv_obj_add_event_cb(object, interaction_event, LV_EVENT_PRESSED, nullptr);
    const std::uint32_t child_count = lv_obj_get_child_count(object);
    for(std::uint32_t index = 0; index < child_count; ++index) {
        install_interaction_wake(lv_obj_get_child(object,
                                                  static_cast<int32_t>(index)));
    }
}

void show_page(moto_ui_page_t page, bool reveal_on_same_page = false) {
    if(page < MOTO_UI_PAGE_NAVIGATION || page >= MOTO_UI_PAGE_COUNT) return;
    if(page == MOTO_UI_PAGE_MUSIC && !ui.music_page_enabled) {
        page = MOTO_UI_PAGE_NAVIGATION;
    }
    const bool changed = page != ui.page;
    ui.page = page;
    { char buf[80]; std::snprintf(buf, sizeof(buf), "show_page -> %d", static_cast<int>(page)); navui_trace(buf); }
    for(int i = 0; i < MOTO_UI_PAGE_COUNT; ++i) {
        if(i == static_cast<int>(page)) {
            lv_obj_remove_flag(ui.pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_page_dots();
    // Navigation state arrives continuously. Only a real page transition or
    // user action may restart the five-second affordance timer.
    if(changed || reveal_on_same_page) reveal_page_dots();
}

void gesture_event(lv_event_t *) {
    lv_indev_t *indev = lv_indev_active();
    if(indev == nullptr) return;
    const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    int next = static_cast<int>(ui.page);
    if(direction == LV_DIR_LEFT) {
        next = (next + 1) % MOTO_UI_PAGE_COUNT;
    } else if(direction == LV_DIR_RIGHT) {
        next = (next + MOTO_UI_PAGE_COUNT - 1) % MOTO_UI_PAGE_COUNT;
    } else {
        return;
    }
    if(!ui.music_page_enabled && next == MOTO_UI_PAGE_MUSIC) {
        next = direction == LV_DIR_LEFT ? MOTO_UI_PAGE_NAVIGATION
                                        : MOTO_UI_PAGE_COMPASS;
    }
    const auto requested = static_cast<moto_ui_page_t>(next);
    // Switch locally first: waiting for the phone's page echo (snapshot
    // throttle + BLE queue) made every fast swipe derive its target from a
    // stale page, which showed up as pages jumping to random indices. The
    // phone echo below is then an idempotent confirmation.
    show_page(requested, true);
    if(ui.page_callback != nullptr) {
        ui.page_callback(requested, ui.page_callback_context);
    }
    reveal_page_dots();
    lv_indev_wait_release(indev);
}

void draw_map_line(lv_layer_t *layer, const lv_draw_line_dsc_t &line) {
    const lv_area_t original_clip = layer->_clip_area;
    lv_area_t visible{
        std::max(original_clip.x1, static_cast<int32_t>(
            std::min(line.p1.x, line.p2.x) - line.width)),
        std::max(original_clip.y1, static_cast<int32_t>(
            std::min(line.p1.y, line.p2.y) - line.width)),
        std::min(original_clip.x2, static_cast<int32_t>(
            std::max(line.p1.x, line.p2.x) + line.width)),
        std::min(original_clip.y2, static_cast<int32_t>(
            std::max(line.p1.y, line.p2.y) + line.width)),
    };
    if(visible.x1 > visible.x2 || visible.y1 > visible.y2) return;
    const float dx = static_cast<float>(line.p2.x - line.p1.x);
    const float dy = static_cast<float>(line.p2.y - line.p1.y);
    constexpr int32_t band_height = 32;
    if(std::abs(dy) <= band_height || std::abs(dx) <= 64.0F ||
       visible.y2 - visible.y1 < band_height) {
        lv_draw_line(layer, &line);
        return;
    }

    // LVGL masks every pixel in a diagonal's bounding rectangle, including
    // the empty space beside a long thin street. Narrow that rectangle per
    // non-overlapping band. Keep the original endpoints so antialiasing and
    // round caps remain identical, including where adjacent bands meet.
    const float slope = dx / dy;
    const float padding = static_cast<float>(line.width) + 2.0F;
    const float minimum_y = static_cast<float>(std::min(line.p1.y, line.p2.y));
    const float maximum_y = static_cast<float>(std::max(line.p1.y, line.p2.y));
    for(int32_t y = visible.y1; y <= visible.y2; y += band_height) {
        lv_area_t band = visible;
        band.y1 = y;
        band.y2 = std::min(y + band_height - 1, visible.y2);
        const float low = std::clamp(static_cast<float>(band.y1) - padding,
                                     minimum_y, maximum_y);
        const float high = std::clamp(static_cast<float>(band.y2) + padding,
                                      minimum_y, maximum_y);
        const float a = line.p1.x + slope * (low - line.p1.y);
        const float b = line.p1.x + slope * (high - line.p1.y);
        band.x1 = std::max(band.x1, static_cast<int32_t>(
            std::floor(std::min(a, b) - padding)));
        band.x2 = std::min(band.x2, static_cast<int32_t>(
            std::ceil(std::max(a, b) + padding)));
        if(band.x1 > band.x2) continue;
        // lv_draw_line copies this clip into its task before dispatching.
        layer->_clip_area = band;
        lv_draw_line(layer, &line);
    }
    layer->_clip_area = original_clip;
}

void draw_map_polyline(lv_event_t *event) {
    lv_obj_t *object = lv_event_get_target_obj(event);
    const auto code = lv_event_get_code(event);
    if(code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        auto *size = static_cast<int32_t *>(lv_event_get_param(event));
        *size = std::max(*size, lv_obj_get_style_line_width(object, LV_PART_MAIN));
        return;
    }
    if(code != LV_EVENT_DRAW_MAIN) return;
    const auto *polyline = static_cast<const MapPolyline *>(
        lv_event_get_user_data(event));
    if(polyline->points == nullptr || polyline->count < 2) return;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t object_area;
    lv_obj_get_coords(object, &object_area);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.base.layer = layer;
    lv_obj_init_draw_line_dsc(object, LV_PART_MAIN, &line);
    for(std::uint16_t index = 1; index < polyline->count; ++index) {
        line.p1 = polyline->points[index - 1];
        line.p2 = polyline->points[index];
        line.p1.x += object_area.x1;
        line.p2.x += object_area.x1;
        line.p1.y += object_area.y1;
        line.p2.y += object_area.y1;
        draw_map_line(layer, line);
        line.round_start = 0;
    }
}

lv_obj_t *create_map_polyline(lv_obj_t *parent, MapPolyline &polyline) {
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(object, &polyline);
    lv_obj_add_event_cb(object, draw_map_polyline, LV_EVENT_ALL, &polyline);
    return object;
}

void set_map_polyline_points(lv_obj_t *object, const lv_point_precise_t *points,
                            std::uint16_t count) {
    auto *polyline = static_cast<MapPolyline *>(lv_obj_get_user_data(object));
    polyline->points = points;
    polyline->count = count;
    lv_obj_invalidate(object);
}

bool apply_route_geometry_frame(bool rebind_lines = true) {
    if(ui.nav_route_point_count < 2) return false;
    bool pixels_changed = false;
    for(std::uint8_t i = 0; i < ui.nav_route_point_count; ++i) {
        const auto x = static_cast<lv_value_precise_t>(
            std::lround(ui.nav_route_x[i]));
        const auto y = static_cast<lv_value_precise_t>(
            std::lround(ui.nav_route_y[i]));
        pixels_changed = pixels_changed || ui.nav_route_points[i].x != x ||
                         ui.nav_route_points[i].y != y;
        ui.nav_route_points[i] = {x, y};
    }
    if(rebind_lines) {
        set_map_polyline_points(ui.nav_route_shadow, ui.nav_route_points,
                                   ui.nav_route_point_count);
        set_map_polyline_points(ui.nav_route, ui.nav_route_points,
                                   ui.nav_route_point_count);
    }
    return pixels_changed || rebind_lines;
}

bool apply_road_geometry_frame(bool rebind_lines = true) {
    if(ui.nav_road_point_count < 2 || ui.nav_road_polyline_count == 0) {
        return false;
    }
    bool pixels_changed = false;
    for(std::uint8_t i = 0; i < ui.nav_road_point_count; ++i) {
        const auto x = static_cast<lv_value_precise_t>(
            std::lround(ui.nav_road_x[i]));
        const auto y = static_cast<lv_value_precise_t>(
            std::lround(ui.nav_road_y[i]));
        pixels_changed = pixels_changed || ui.nav_road_points[i].x != x ||
                         ui.nav_road_points[i].y != y;
        ui.nav_road_points[i] = {x, y};
    }
    if(rebind_lines) {
        for(std::uint8_t i = 0; i < ui.nav_road_polyline_count; ++i) {
            const moto_ui_polyline_span_t span = ui.nav_road_spans[i];
            set_map_polyline_points(
                ui.nav_roads[i],
                &ui.nav_road_points[span.first_point_index],
                span.point_count);
        }
    }
    return pixels_changed || rebind_lines;
}

bool apply_building_geometry_frame(bool rebind_lines = true) {
    if(ui.nav_building_point_count < 3 ||
       ui.nav_building_footprint_count == 0) return false;
    bool pixels_changed = false;
    std::size_t packed_index = 0;
    for(std::uint8_t footprint_index = 0;
        footprint_index < ui.nav_building_footprint_count;
        ++footprint_index) {
        const moto_ui_building_span_t span =
            ui.nav_building_spans[footprint_index];
        for(std::uint8_t point_index = 0; point_index < span.point_count;
            ++point_index) {
            const std::size_t source = span.first_point_index + point_index;
            const auto x = static_cast<lv_value_precise_t>(
                std::lround(ui.nav_building_x[source]));
            const auto y = static_cast<lv_value_precise_t>(
                std::lround(ui.nav_building_y[source]));
            pixels_changed = pixels_changed ||
                ui.nav_building_points[packed_index].x != x ||
                ui.nav_building_points[packed_index].y != y;
            ui.nav_building_points[packed_index++] = {x, y};
        }
        // BLE omits the duplicate closing point; LVGL needs it explicitly.
        const auto closing =
            ui.nav_building_points[packed_index - span.point_count];
        pixels_changed = pixels_changed ||
            ui.nav_building_points[packed_index].x != closing.x ||
            ui.nav_building_points[packed_index].y != closing.y;
        ui.nav_building_points[packed_index] = closing;
        if(rebind_lines) {
            set_map_polyline_points(
                ui.nav_buildings[footprint_index],
                &ui.nav_building_points[packed_index - span.point_count],
                span.point_count + 1U);
        }
        ++packed_index;
    }
    return pixels_changed || rebind_lines;
}

// Map layers carry no opaque background, so hiding a polyline only hides the
// line object: the pixels it painted stay in the framebuffer until something
// repaints that area. Every geometry add/clear therefore repaints the whole
// map viewport, which keeps a clear-then-redraw cycle bit-exact.
void invalidate_map_region() {
    if(ui.nav_map != nullptr) {
        lv_obj_invalidate(ui.nav_map);
    }
}

void route_motion_tick(lv_timer_t *) {
    if(ui.nav_route_target_count < 2 || ui.nav_route_point_count < 2) return;
    // 0.39 at 25 ms has approximately the same smoothing time constant as
    // the old 0.56 at 40 ms, but supplies smaller and more frequent steps.
    // Screen coordinates need subpixel precision, not geographic doubles.
    // Float uses the ESP32-S3 FPU for every point in this 40 Hz hot loop.
    const float blend = ui.reduce_motion ? 1.0F : 0.39F;
    bool route_moved = false;
    for(std::uint8_t i = 0; i < ui.nav_route_point_count; ++i) {
        const float dx = ui.nav_route_target_x[i] - ui.nav_route_x[i];
        const float dy = ui.nav_route_target_y[i] - ui.nav_route_y[i];
        if(std::abs(dx) < 0.08F && std::abs(dy) < 0.08F) {
            ui.nav_route_x[i] = ui.nav_route_target_x[i];
            ui.nav_route_y[i] = ui.nav_route_target_y[i];
            continue;
        }
        ui.nav_route_x[i] += dx * blend;
        ui.nav_route_y[i] += dy * blend;
        route_moved = true;
    }
    bool roads_moved = false;
    for(std::uint8_t i = 0; i < ui.nav_road_point_count; ++i) {
        const float dx = ui.nav_road_target_x[i] - ui.nav_road_x[i];
        const float dy = ui.nav_road_target_y[i] - ui.nav_road_y[i];
        if(std::abs(dx) < 0.08F && std::abs(dy) < 0.08F) {
            ui.nav_road_x[i] = ui.nav_road_target_x[i];
            ui.nav_road_y[i] = ui.nav_road_target_y[i];
            continue;
        }
        ui.nav_road_x[i] += dx * blend;
        ui.nav_road_y[i] += dy * blend;
        roads_moved = true;
    }
    bool buildings_moved = false;
    for(std::uint8_t i = 0; i < ui.nav_building_point_count; ++i) {
        const float dx = ui.nav_building_target_x[i] - ui.nav_building_x[i];
        const float dy = ui.nav_building_target_y[i] - ui.nav_building_y[i];
        if(std::abs(dx) < 0.08F && std::abs(dy) < 0.08F) {
            ui.nav_building_x[i] = ui.nav_building_target_x[i];
            ui.nav_building_y[i] = ui.nav_building_target_y[i];
            continue;
        }
        ui.nav_building_x[i] += dx * blend;
        ui.nav_building_y[i] += dy * blend;
        buildings_moved = true;
    }
    // The line objects retain pointers to the mutable point arrays. Ordinary
    // animation therefore only changes those arrays and invalidates the one
    // common map layer. Rebinding every road/building used to invalidate the
    // same 466x232 region dozens of times per frame.
    bool pixels_changed = false;
    if(buildings_moved) {
        pixels_changed = apply_building_geometry_frame(false) || pixels_changed;
    }
    if(roads_moved) {
        pixels_changed = apply_road_geometry_frame(false) || pixels_changed;
    }
    if(route_moved) {
        pixels_changed = apply_route_geometry_frame(false) || pixels_changed;
    }
    if(pixels_changed) lv_obj_invalidate(ui.nav_map);
}

void update_road_geometry(const moto_ui_state_t *state) {
    const std::uint8_t point_count = std::min<std::uint8_t>(
        state->road_point_count, MOTO_UI_ROAD_POINT_CAPACITY);
    const std::uint8_t polyline_count = std::min<std::uint8_t>(
        state->road_polyline_count, MOTO_UI_ROAD_POLYLINE_CAPACITY);
    if(point_count < 2 || polyline_count == 0) {
        ui.nav_road_point_count = 0;
        ui.nav_road_polyline_count = 0;
        ui.nav_road_scene_revision = state->map_scene_revision;
        for(lv_obj_t *road : ui.nav_roads) {
            lv_obj_add_flag(road, LV_OBJ_FLAG_HIDDEN);
        }
        invalidate_map_region();
        return;
    }

    bool spans_changed =
        ui.nav_road_scene_revision != state->map_scene_revision ||
        ui.nav_road_point_count != point_count ||
        ui.nav_road_polyline_count != polyline_count;
    for(std::uint8_t i = 0; i < point_count; ++i) {
        ui.nav_road_target_x[i] = state->road_points[i].x;
        ui.nav_road_target_y[i] = state->road_points[i].y;
    }
    for(std::uint8_t i = 0; i < polyline_count; ++i) {
        const moto_ui_polyline_span_t next = state->road_polylines[i];
        spans_changed = spans_changed ||
                        ui.nav_road_spans[i].first_point_index !=
                            next.first_point_index ||
                        ui.nav_road_spans[i].point_count != next.point_count ||
                        ui.nav_road_spans[i].road_class != next.road_class;
        ui.nav_road_spans[i] = next;
    }

    // A wide point can move more than 96 px during a perfectly ordinary fast
    // yaw. Treating screen-space distance as a route replacement made the
    // whole map jump. Only topology/revision changes snap; motion interpolates.
    const bool snap = spans_changed || ui.reduce_motion;
    ui.nav_road_point_count = point_count;
    ui.nav_road_polyline_count = polyline_count;
    ui.nav_road_scene_revision = state->map_scene_revision;
    if(snap) {
        for(std::uint8_t i = 0; i < point_count; ++i) {
            ui.nav_road_x[i] = ui.nav_road_target_x[i];
            ui.nav_road_y[i] = ui.nav_road_target_y[i];
        }
    }
    if(spans_changed) {
        for(std::uint8_t i = 0; i < MOTO_UI_ROAD_POLYLINE_CAPACITY; ++i) {
            if(i < polyline_count) {
                const std::uint8_t road_class = ui.nav_road_spans[i].road_class;
                const bool major = road_class <= 2U;
                const bool service = road_class >= 4U;
                lv_obj_set_style_line_width(
                    ui.nav_roads[i], px(major ? 4 : (service ? 2 : 3)), 0);
                lv_obj_set_style_line_color(
                    ui.nav_roads[i],
                    major ? kRoadMajor : (service ? kRoadMinor : kRoadGray), 0);
                lv_obj_remove_flag(ui.nav_roads[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(ui.nav_roads[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if(snap) {
        apply_road_geometry_frame();
        invalidate_map_region();
    }
}


void update_building_geometry(const moto_ui_state_t *state) {
    const std::uint8_t point_count = std::min<std::uint8_t>(
        state->building_point_count, MOTO_UI_BUILDING_POINT_CAPACITY);
    const std::uint8_t footprint_count = std::min<std::uint8_t>(
        state->building_footprint_count,
        MOTO_UI_BUILDING_FOOTPRINT_CAPACITY);
    if(point_count < 3 || footprint_count == 0) {
        ui.nav_building_point_count = 0;
        ui.nav_building_footprint_count = 0;
        ui.nav_building_scene_revision = state->map_scene_revision;
        for(lv_obj_t *building : ui.nav_buildings) {
            lv_obj_add_flag(building, LV_OBJ_FLAG_HIDDEN);
        }
        invalidate_map_region();
        return;
    }

    bool spans_changed =
        ui.nav_building_scene_revision != state->map_scene_revision ||
        ui.nav_building_point_count != point_count ||
        ui.nav_building_footprint_count != footprint_count;
    for(std::uint8_t i = 0; i < point_count; ++i) {
        ui.nav_building_target_x[i] = state->building_points[i].x;
        ui.nav_building_target_y[i] = state->building_points[i].y;
    }
    for(std::uint8_t i = 0; i < footprint_count; ++i) {
        const moto_ui_building_span_t next = state->building_footprints[i];
        spans_changed = spans_changed ||
                        ui.nav_building_spans[i].first_point_index !=
                            next.first_point_index ||
                        ui.nav_building_spans[i].point_count !=
                            next.point_count ||
                        ui.nav_building_spans[i].building_class !=
                            next.building_class;
        ui.nav_building_spans[i] = next;
    }

    const bool snap = spans_changed || ui.reduce_motion;
    ui.nav_building_point_count = point_count;
    ui.nav_building_footprint_count = footprint_count;
    ui.nav_building_scene_revision = state->map_scene_revision;
    if(snap) {
        for(std::uint8_t i = 0; i < point_count; ++i) {
            ui.nav_building_x[i] = ui.nav_building_target_x[i];
            ui.nav_building_y[i] = ui.nav_building_target_y[i];
        }
    }
    if(spans_changed) {
        for(std::uint8_t i = 0;
            i < MOTO_UI_BUILDING_FOOTPRINT_CAPACITY; ++i) {
            if(i < footprint_count) {
                const bool landmark =
                    ui.nav_building_spans[i].building_class == 1U;
                lv_obj_set_style_line_color(
                    ui.nav_buildings[i],
                    landmark ? kBuildingLandmark : kBuildingGray, 0);
                lv_obj_set_style_line_width(
                    ui.nav_buildings[i], px(landmark ? 2 : 1), 0);
                lv_obj_remove_flag(ui.nav_buildings[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(ui.nav_buildings[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if(snap) {
        apply_building_geometry_frame();
        invalidate_map_region();
    }
}

void update_route_geometry(const moto_ui_state_t *state) {
    const std::uint8_t count = std::min<std::uint8_t>(
        state->route_point_count, MOTO_UI_ROUTE_POINT_CAPACITY);
    if(count < 2) {
        ui.nav_route_point_count = 0;
        ui.nav_route_target_count = 0;
        ui.nav_route_identity = state->route_identity;
        ui.nav_route_generation = state->route_generation;
        lv_obj_add_flag(ui.nav_route_shadow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.nav_route, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.nav_marker, LV_OBJ_FLAG_HIDDEN);
        // Hiding a child does not repaint the parent's background: the route
        // pixels would survive until the next unrelated redraw. Invalidate the
        // route layer so the cleared frame really is cleared.
        invalidate_map_region();
        return;
    }

    for(std::uint8_t i = 0; i < count; ++i) {
        ui.nav_route_target_x[i] = state->route_points[i].x;
        ui.nav_route_target_y[i] = state->route_points[i].y;
    }

    // A new route or a wholesale reroute should appear immediately. Ordinary
    // GNSS/IMU updates are blended at 40 Hz so the road glides under the fixed
    // rider marker instead of jumping from one phone fix to the next.
    const bool snap = ui.nav_route_identity != state->route_identity ||
                      ui.nav_route_generation != state->route_generation ||
                      ui.nav_route_point_count != count || ui.reduce_motion;
    ui.nav_route_target_count = count;
    ui.nav_route_point_count = count;
    ui.nav_route_identity = state->route_identity;
    ui.nav_route_generation = state->route_generation;
    if(snap) {
        for(std::uint8_t i = 0; i < count; ++i) {
            ui.nav_route_x[i] = ui.nav_route_target_x[i];
            ui.nav_route_y[i] = ui.nav_route_target_y[i];
        }
    }

    lv_obj_remove_flag(ui.nav_route_shadow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui.nav_route, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui.nav_marker, LV_OBJ_FLAG_HIDDEN);
    if(snap) {
        apply_route_geometry_frame();
        invalidate_map_region();
    }
}

lv_point_precise_t arrow_base(const lv_point_precise_t& previous,
                               const lv_point_precise_t& tip,
                               double head_length = px(24.0),
                               double scale = 1.0) {
    const double dx = static_cast<double>(tip.x - previous.x);
    const double dy = static_cast<double>(tip.y - previous.y);
    const double length = std::max(1.0, std::hypot(dx, dy));
    const double ux = dx / length;
    const double uy = dy / length;
    return {
        static_cast<lv_value_precise_t>(tip.x - ux * head_length * scale),
        static_cast<lv_value_precise_t>(tip.y - uy * head_length * scale),
    };
}

void draw_arrowhead(lv_layer_t *layer, const lv_area_t& area,
                     const lv_point_precise_t& previous,
                     const lv_point_precise_t& tip, double scale = 1.0) {
    const double dx = static_cast<double>(tip.x - previous.x);
    const double dy = static_cast<double>(tip.y - previous.y);
    const double length = std::max(1.0, std::hypot(dx, dy));
    const double ux = dx / length;
    const double uy = dy / length;
    const auto base = arrow_base(previous, tip, px(24.0), scale);
    const double wing = px(18.0) * scale;

    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.color = kWhite;
    triangle.opa = LV_OPA_COVER;
    triangle.p[0] = {
        static_cast<lv_value_precise_t>(area.x1 + tip.x),
        static_cast<lv_value_precise_t>(area.y1 + tip.y),
    };
    triangle.p[1] = {
        static_cast<lv_value_precise_t>(area.x1 + base.x - uy * wing),
        static_cast<lv_value_precise_t>(area.y1 + base.y + ux * wing),
    };
    triangle.p[2] = {
        static_cast<lv_value_precise_t>(area.x1 + base.x + uy * wing),
        static_cast<lv_value_precise_t>(area.y1 + base.y - ux * wing),
    };
    lv_draw_triangle(layer, &triangle);
}
void draw_maneuver_icon(lv_event_t *event) {
    lv_obj_t *object = lv_event_get_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    lv_layer_t *layer = lv_event_get_layer(event);

    // §4.3: the S/M/L tiers scale the same authored artwork, so every glyph
    // coordinate and stroke width is multiplied by the current tier factor.
    const double scale = ui.maneuver_scale;
    const auto gx = [scale](double value) {
        return static_cast<lv_value_precise_t>(
            std::lround(value * scale * MOTO_UI_CANVAS_WIDTH / 360.0));
    };
    const auto gw = [scale](double value) {
        return static_cast<int32_t>(std::lround(
            value * scale * MOTO_UI_CANVAS_WIDTH / 360.0));
    };

    lv_point_precise_t points[8]{};
    int count = 0;
    switch(ui.nav_maneuver_type) {
        case MOTO_MANEUVER_RIGHT:
            points[0] = {gx(34), gx(102)};
            points[1] = {gx(34), gx(63)};
            points[2] = {gx(55), gx(42)};
            points[3] = {gx(105), gx(42)};
            count = 4;
            break;
        case MOTO_MANEUVER_LEFT:
            points[0] = {gx(82), gx(102)};
            points[1] = {gx(82), gx(63)};
            points[2] = {gx(61), gx(42)};
            points[3] = {gx(11), gx(42)};
            count = 4;
            break;
        case MOTO_MANEUVER_SLIGHT_RIGHT:
            points[0] = {gx(36), gx(102)};
            points[1] = {gx(36), gx(72)};
            points[2] = {gx(96), gx(12)};
            count = 3;
            break;
        case MOTO_MANEUVER_SLIGHT_LEFT:
            points[0] = {gx(80), gx(102)};
            points[1] = {gx(80), gx(72)};
            points[2] = {gx(20), gx(12)};
            count = 3;
            break;
        case MOTO_MANEUVER_UTURN:
            points[0] = {gx(88), gx(102)};
            points[1] = {gx(88), gx(50)};
            points[2] = {gx(82), gx(33)};
            points[3] = {gx(69), gx(23)};
            points[4] = {gx(51), gx(20)};
            points[5] = {gx(34), gx(27)};
            points[6] = {gx(25), gx(43)};
            points[7] = {gx(25), gx(84)};
            count = 8;
            break;
        case MOTO_MANEUVER_ROUNDABOUT: {
            lv_draw_line_dsc_t stem;
            lv_draw_line_dsc_init(&stem);
            stem.color = kWhite;
            stem.width = gw(11);
            stem.round_start = 1;
            stem.round_end = 1;
            stem.p1 = {static_cast<lv_value_precise_t>(area.x1 + gx(58)),
                       static_cast<lv_value_precise_t>(area.y1 + gx(101))};
            stem.p2 = {static_cast<lv_value_precise_t>(area.x1 + gx(58)),
                       static_cast<lv_value_precise_t>(area.y1 + gx(79))};
            lv_draw_line(layer, &stem);

            lv_draw_arc_dsc_t circle;
            lv_draw_arc_dsc_init(&circle);
            circle.color = kWhite;
            circle.width = gw(11);
            circle.rounded = 1;
            circle.center = {
                static_cast<int32_t>(area.x1 + gx(58)),
                static_cast<int32_t>(area.y1 + gx(53)),
            };
            circle.radius = static_cast<int32_t>(gx(27));
            circle.start_angle = 86;
            circle.end_angle = 326;
            lv_draw_arc(layer, &circle);

            const lv_point_precise_t previous = {gx(69), gx(24)};
            const lv_point_precise_t tip = {gx(86), gx(36)};
            draw_arrowhead(layer, area, previous, tip, scale);
            return;
        }
        case MOTO_MANEUVER_ARRIVE: {
            lv_draw_line_dsc_t pole;
            lv_draw_line_dsc_init(&pole);
            pole.color = kWhite;
            pole.width = gw(11);
            pole.round_start = 1;
            pole.round_end = 1;
            pole.p1 = {static_cast<lv_value_precise_t>(area.x1 + gx(42)),
                       static_cast<lv_value_precise_t>(area.y1 + gx(101))};
            pole.p2 = {static_cast<lv_value_precise_t>(area.x1 + gx(42)),
                       static_cast<lv_value_precise_t>(area.y1 + gx(17))};
            lv_draw_line(layer, &pole);

            lv_draw_triangle_dsc_t flag;
            lv_draw_triangle_dsc_init(&flag);
            flag.color = kWhite;
            flag.opa = LV_OPA_COVER;
            flag.p[0] = {
                static_cast<lv_value_precise_t>(area.x1 + gx(46)),
                static_cast<lv_value_precise_t>(area.y1 + gx(20)),
            };
            flag.p[1] = {
                static_cast<lv_value_precise_t>(area.x1 + gx(102)),
                static_cast<lv_value_precise_t>(area.y1 + gx(39)),
            };
            flag.p[2] = {
                static_cast<lv_value_precise_t>(area.x1 + gx(46)),
                static_cast<lv_value_precise_t>(area.y1 + gx(58)),
            };
            lv_draw_triangle(layer, &flag);
            return;
        }
        case MOTO_MANEUVER_STRAIGHT:
        default:
            points[0] = {gx(58), gx(103)};
            points[1] = {gx(58), gx(10)};
            count = 2;
            break;
    }

    lv_point_precise_t absolute[8]{};
    const lv_point_precise_t base = arrow_base(points[count - 2],
                                                points[count - 1]);
    for(int i = 0; i < count; ++i) {
        const lv_point_precise_t point = i == count - 1 ? base : points[i];
        absolute[i] = {
            static_cast<lv_value_precise_t>(area.x1 + point.x),
            static_cast<lv_value_precise_t>(area.y1 + point.y),
        };
    }
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = kWhite;
    line.width = gw(12);
    line.round_start = 1;
    line.round_end = 1;
    line.points = absolute;
    line.point_cnt = count;
    lv_draw_line(layer, &line);
    draw_arrowhead(layer, area, points[count - 2], points[count - 1], scale);
}

void update_maneuver(moto_maneuver_t maneuver) {
    if(ui.nav_maneuver_type == maneuver) return;
    ui.nav_maneuver_type = maneuver;
    lv_obj_invalidate(ui.nav_maneuver);
}

void draw_segment(lv_layer_t *layer, lv_color_t color, int width,
                  lv_opa_t opacity, const lv_point_precise_t *points,
                  int count) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = width;
    line.opa = opacity;
    line.round_start = 1;
    line.round_end = 1;
    // LVGL's descriptor predates const-correct polyline inputs; the draw task
    // only reads the points for the duration of this call.
    line.points = const_cast<lv_point_precise_t *>(points);
    line.point_cnt = count;
    lv_draw_line(layer, &line);
}

void draw_disc(lv_layer_t *layer, int x, int y, int diameter,
               lv_color_t color, lv_opa_t opacity) {
    lv_draw_rect_dsc_t disc;
    lv_draw_rect_dsc_init(&disc);
    disc.radius = LV_RADIUS_CIRCLE;
    disc.bg_color = color;
    disc.bg_opa = opacity;
    disc.border_opa = LV_OPA_TRANSP;
    const lv_area_t area = {
        x - diameter / 2,
        y - diameter / 2,
        x + diameter / 2,
        y + diameter / 2,
    };
    lv_draw_rect(layer, &disc, &area);
}

void draw_phone_body(lv_layer_t *layer, const lv_area_t& area,
                     lv_color_t color, lv_opa_t opacity) {
    const lv_area_t phone = {
        area.x1 + px(49),
        area.y1 + px(25),
        area.x1 + px(101),
        area.y1 + px(121),
    };
    lv_draw_rect_dsc_t frame;
    lv_draw_rect_dsc_init(&frame);
    frame.radius = px(13);
    frame.bg_opa = LV_OPA_TRANSP;
    frame.border_color = color;
    frame.border_width = px(4);
    frame.border_opa = opacity;
    lv_draw_rect(layer, &frame, &phone);

    const lv_point_precise_t receiver[] = {
        {static_cast<lv_value_precise_t>(area.x1 + px(67)),
         static_cast<lv_value_precise_t>(area.y1 + px(36))},
        {static_cast<lv_value_precise_t>(area.x1 + px(83)),
         static_cast<lv_value_precise_t>(area.y1 + px(36))},
    };
    const lv_point_precise_t home[] = {
        {static_cast<lv_value_precise_t>(area.x1 + px(68)),
         static_cast<lv_value_precise_t>(area.y1 + px(110))},
        {static_cast<lv_value_precise_t>(area.x1 + px(82)),
         static_cast<lv_value_precise_t>(area.y1 + px(110))},
    };
    draw_segment(layer, color, px(3), opacity, receiver, 2);
    draw_segment(layer, color, px(3), opacity, home, 2);
}

void draw_connection_symbol(lv_event_t *event) {
    lv_obj_t *object = lv_event_get_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    const int center_x = area.x1 + px(75);
    const int center_y = area.y1 + px(73);
    const double phase = ui.lifecycle_phase_deg * kPi / 180.0;

    if(ui.lifecycle_visual == LifecycleVisual::Connected) {
        const int pulse = static_cast<int>(px(67) +
            std::lround((std::sin(phase) + 1.0) * px(3.0)));
        lv_draw_arc_dsc_t halo;
        lv_draw_arc_dsc_init(&halo);
        halo.color = kGreen;
        halo.width = px(3);
        halo.opa = LV_OPA_50;
        halo.rounded = 1;
        halo.center = {center_x, center_y};
        halo.radius = pulse;
        halo.start_angle = 0;
        halo.end_angle = 360;
        lv_draw_arc(layer, &halo);
        draw_disc(layer, center_x, center_y, px(92), kGreen, LV_OPA_COVER);
        const lv_point_precise_t check[] = {
            {static_cast<lv_value_precise_t>(area.x1 + px(53)),
             static_cast<lv_value_precise_t>(area.y1 + px(74))},
            {static_cast<lv_value_precise_t>(area.x1 + px(69)),
             static_cast<lv_value_precise_t>(area.y1 + px(89))},
            {static_cast<lv_value_precise_t>(area.x1 + px(99)),
             static_cast<lv_value_precise_t>(area.y1 + px(55))},
        };
        draw_segment(layer, kBlack, px(9), LV_OPA_COVER, check, 3);
        return;
    }

    if(ui.lifecycle_visual == LifecycleVisual::Ready ||
       ui.lifecycle_visual == LifecycleVisual::Planning) {
        const lv_point_precise_t route[] = {
            {static_cast<lv_value_precise_t>(area.x1 + px(34)),
             static_cast<lv_value_precise_t>(area.y1 + px(113))},
            {static_cast<lv_value_precise_t>(area.x1 + px(50)),
             static_cast<lv_value_precise_t>(area.y1 + px(91))},
            {static_cast<lv_value_precise_t>(area.x1 + px(80)),
             static_cast<lv_value_precise_t>(area.y1 + px(98))},
            {static_cast<lv_value_precise_t>(area.x1 + px(111)),
             static_cast<lv_value_precise_t>(area.y1 + px(48))},
        };
        draw_segment(layer, kGraphite, px(12), LV_OPA_COVER, route, 4);
        draw_segment(layer, kWhite, px(5), LV_OPA_COVER, route, 4);
        draw_disc(layer, route[0].x, route[0].y, px(16), kWhite,
                  LV_OPA_COVER);

        lv_draw_arc_dsc_t pin;
        lv_draw_arc_dsc_init(&pin);
        pin.color = kWhite;
        pin.width = px(5);
        pin.opa = LV_OPA_COVER;
        pin.rounded = 1;
        pin.center = {route[3].x, route[3].y};
        pin.radius = px(13);
        pin.start_angle = 0;
        pin.end_angle = 360;
        lv_draw_arc(layer, &pin);
        draw_disc(layer, route[3].x, route[3].y, px(7), kWhite,
                  LV_OPA_COVER);

        if(ui.lifecycle_visual == LifecycleVisual::Planning) {
            // A single travelling bead communicates progress without a
            // processor-heavy full-screen animation.
            const double t =
                static_cast<double>(ui.lifecycle_phase_deg % 120U) / 120.0;
            const int segment = std::min(2, static_cast<int>(t * 3.0));
            const double local = t * 3.0 - segment;
            const int x = static_cast<int>(std::lround(
                route[segment].x +
                (route[segment + 1].x - route[segment].x) * local));
            const int y = static_cast<int>(std::lround(
                route[segment].y +
                (route[segment + 1].y - route[segment].y) * local));
            draw_disc(layer, x, y, px(12), kWhite, LV_OPA_COVER);
        } else {
            const lv_opa_t ready_opa = static_cast<lv_opa_t>(
                150 + std::lround((std::sin(phase) + 1.0) * 40.0));
            draw_disc(layer, route[0].x, route[0].y, px(25), kWhite,
                      ready_opa);
        }
        return;
    }

    draw_phone_body(layer, area, kWhite, LV_OPA_COVER);
    const bool connecting =
        ui.lifecycle_visual == LifecycleVisual::PhoneConnecting;
    const int speed = connecting ? 2 : 1;
    for(int index = 0; index < 3; ++index) {
        lv_draw_arc_dsc_t scan;
        lv_draw_arc_dsc_init(&scan);
        scan.color = index == 0 ? kWhite : kQuiet;
        scan.width = px(index == 0 ? 4 : 3);
        scan.opa = static_cast<lv_opa_t>(220 - index * 55);
        scan.rounded = 1;
        scan.center = {center_x, center_y};
        scan.radius = px(66 - index * 7);
        const int start =
            (static_cast<int>(ui.lifecycle_phase_deg) * speed + index * 112) %
            360;
        scan.start_angle = start;
        scan.end_angle = start + (connecting ? 54 : 34);
        lv_draw_arc(layer, &scan);
    }
    const int orbit_x = static_cast<int>(
        std::lround(center_x + std::sin(phase * speed) * px(66.0)));
    const int orbit_y = static_cast<int>(
        std::lround(center_y - std::cos(phase * speed) * px(66.0)));
    draw_disc(layer, orbit_x, orbit_y, px(connecting ? 11 : 9), kWhite,
              LV_OPA_COVER);
}

void set_layer_opacity(void *object, int32_t opacity) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), opacity, 0);
}

void apply_lifecycle_content(LifecycleVisual visual) {
    ui.lifecycle_visual = visual;
    const char *kicker = "MOTO GPS / 手机连接";
    const char *title = "";
    const char *subtitle = "";
    lv_color_t title_color = kWhite;
    switch(visual) {
        case LifecycleVisual::PhoneOffline:
            title = "连接手机";
            subtitle = "请打开手机应用";
            break;
        case LifecycleVisual::PhoneConnecting:
            title = "正在连接";
            subtitle = "正在建立连接";
            break;
        case LifecycleVisual::Connected:
            title = "已连接";
            subtitle = "连接成功";
            title_color = kGreen;
            break;
        case LifecycleVisual::Ready:
            kicker = "MOTO GPS / 就绪";
            title = "准备出发";
            subtitle = "请在手机选择目的地";
            break;
        case LifecycleVisual::Planning:
            kicker = "MOTO GPS / 路线";
            title = "规划路线";
            subtitle = "正在规划路线";
            break;
        case LifecycleVisual::Hidden:
            break;
    }
    lv_label_set_text(ui.nav_lifecycle_kicker, kicker);
    lv_label_set_text(ui.nav_lifecycle_title, title);
    lv_label_set_text(ui.nav_lifecycle_subtitle, subtitle);
    lv_obj_set_style_text_color(ui.nav_lifecycle_title, title_color, 0);
    lv_obj_set_style_text_color(ui.nav_lifecycle_subtitle,
                                visual == LifecycleVisual::Connected
                                    ? kGreen : kSoft,
                                0);
    lv_obj_invalidate(ui.nav_lifecycle_symbol);
    { char buf[160]; std::snprintf(buf, sizeof(buf), "lifecycle visual=%d kicker=[%s] title=[%s]", static_cast<int>(visual), kicker, title); navui_trace(buf); }
}

void lifecycle_fade_in();

void lifecycle_fade_out_complete(lv_anim_t *) {
    if(ui.nav_lifecycle == nullptr) return;
    if(ui.lifecycle_target == LifecycleVisual::Hidden) {
        ui.lifecycle_visual = LifecycleVisual::Hidden;
        lv_obj_add_flag(ui.nav_lifecycle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(ui.nav_lifecycle, LV_OPA_COVER, 0);
        return;
    }
    apply_lifecycle_content(ui.lifecycle_target);
    lifecycle_fade_in();
}

void lifecycle_fade_in() {
    lv_obj_remove_flag(ui.nav_lifecycle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(ui.nav_lifecycle, LV_OPA_TRANSP, 0);
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, ui.nav_lifecycle);
    lv_anim_set_exec_cb(&fade, set_layer_opacity);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade, 220);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_start(&fade);
}

void transition_lifecycle(LifecycleVisual next) {
    if(ui.nav_lifecycle == nullptr) return;
    if(next == ui.lifecycle_target &&
       (next == ui.lifecycle_visual ||
        lv_obj_has_flag(ui.nav_lifecycle, LV_OBJ_FLAG_HIDDEN))) {
        return;
    }
    ui.lifecycle_target = next;
    lv_anim_delete(ui.nav_lifecycle, set_layer_opacity);
    if(ui.reduce_motion) {
        if(next == LifecycleVisual::Hidden) {
            ui.lifecycle_visual = LifecycleVisual::Hidden;
            lv_obj_add_flag(ui.nav_lifecycle, LV_OBJ_FLAG_HIDDEN);
        } else {
            apply_lifecycle_content(next);
            lv_obj_remove_flag(ui.nav_lifecycle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(ui.nav_lifecycle, LV_OPA_COVER, 0);
        }
        return;
    }
    if(ui.lifecycle_visual == LifecycleVisual::Hidden ||
       lv_obj_has_flag(ui.nav_lifecycle, LV_OBJ_FLAG_HIDDEN)) {
        if(next == LifecycleVisual::Hidden) return;
        apply_lifecycle_content(next);
        lifecycle_fade_in();
        return;
    }

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, ui.nav_lifecycle);
    lv_anim_set_exec_cb(&fade, set_layer_opacity);
    lv_anim_set_values(&fade,
                       lv_obj_get_style_opa(ui.nav_lifecycle, LV_PART_MAIN),
                       LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, 140);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&fade, lifecycle_fade_out_complete);
    lv_anim_start(&fade);
}

LifecycleVisual desired_lifecycle_visual() {
    if(ui.demo_active) {
        return LifecycleVisual::Hidden;
    }
    if(ui.phone_connection == MOTO_UI_PHONE_OFFLINE) {
        return LifecycleVisual::PhoneOffline;
    }
    if(ui.phone_connection == MOTO_UI_PHONE_CONNECTING) {
        return LifecycleVisual::PhoneConnecting;
    }
    if(ui.lifecycle_success_active) {
        return LifecycleVisual::Connected;
    }
    if(ui.navigation_has_guidance) {
        return LifecycleVisual::Hidden;
    }
    if(ui.navigation_route_request_in_flight) {
        return LifecycleVisual::Planning;
    }
    return LifecycleVisual::Ready;
}

void refresh_lifecycle() {
    transition_lifecycle(desired_lifecycle_visual());
}

void lifecycle_tick(lv_timer_t *) {
    if(ui.reduce_motion || ui.nav_lifecycle_symbol == nullptr ||
       ui.lifecycle_visual == LifecycleVisual::Hidden) {
        return;
    }
    const std::uint16_t step =
        ui.lifecycle_visual == LifecycleVisual::PhoneConnecting ? 9U : 5U;
    ui.lifecycle_phase_deg =
        static_cast<std::uint16_t>((ui.lifecycle_phase_deg + step) % 360U);
    lv_obj_invalidate(ui.nav_lifecycle_symbol);
}

void connection_success_timeout(lv_timer_t *timer) {
    lv_timer_pause(timer);
    ui.lifecycle_success_active = false;
    refresh_lifecycle();
}

// Guidance visibility is the AND of the layout switch and data availability:
// a component the user switched off must never be resurrected by guidance,
// and one with no route must disappear even when the layout enables it.
void set_component_visible(lv_obj_t *object, bool layout_enabled,
                           bool data_available) {
    if(object == nullptr) return;
    if(layout_enabled && data_available) {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_navigation_guidance_visible(bool visible) {
    const moto_ui_component_layout_t *maneuver =
        find_component(ui.layout, MOTO_UI_COMPONENT_MANEUVER);
    const moto_ui_component_layout_t *remaining_distance =
        find_component(ui.layout, MOTO_UI_COMPONENT_REMAINING_DISTANCE);
    const moto_ui_component_layout_t *remaining_time =
        find_component(ui.layout, MOTO_UI_COMPONENT_REMAINING_TIME);
    const moto_ui_component_layout_t *progress =
        find_component(ui.layout, MOTO_UI_COMPONENT_PROGRESS_ARC);

    set_component_visible(ui.nav_maneuver,
                          maneuver != nullptr && maneuver->enabled, visible);
    set_component_visible(ui.nav_distance,
                          maneuver != nullptr && maneuver->enabled, visible);
    set_component_visible(ui.nav_unit,
                          maneuver != nullptr && maneuver->enabled, visible);
    set_component_visible(ui.nav_progress,
                          progress != nullptr && progress->enabled, visible);
    set_component_visible(ui.nav_remaining_distance,
                          remaining_distance != nullptr &&
                              remaining_distance->enabled,
                          visible);
    set_component_visible(ui.nav_remaining_distance_caption,
                          remaining_distance != nullptr &&
                              remaining_distance->enabled,
                          visible);
    set_component_visible(ui.nav_remaining_time,
                          remaining_time != nullptr && remaining_time->enabled,
                          visible);
    set_component_visible(ui.nav_remaining_time_caption,
                          remaining_time != nullptr && remaining_time->enabled,
                          visible);
    if(!visible) lv_obj_add_flag(ui.nav_limit, LV_OBJ_FLAG_HIDDEN);
}

// The acceleration readout hugs the speed arc band, so its position follows
// the arc anchor rather than the per-frame text updates: aligning here keeps a
// label whose glyph width never changes bit-exact between frames.
void align_accel_readout() {
    if(ui.nav_accel == nullptr || ui.nav_speed_arc_object == nullptr) return;
    const moto_ui_component_layout_t *arc =
        find_component(ui.layout, MOTO_UI_COMPONENT_SPEED_ARC);
    if(arc == nullptr) return;
    const int offset = 408 / 6;
    int dx = 0, dy = 0;
    switch(arc->anchor) {
        case MOTO_UI_ARC_TOP:    dy = -offset; break;
        case MOTO_UI_ARC_BOTTOM: dy = offset; break;
        case MOTO_UI_ARC_LEFT:   dx = -offset; break;
        case MOTO_UI_ARC_RIGHT:  dx = offset; break;
        default: break;
    }
    lv_obj_align_to(ui.nav_accel, ui.nav_speed_arc_object, LV_ALIGN_CENTER,
                    dx, dy);
    // The readout is centre aligned inside a fixed-width box: a sign change or
    // an extra digit must not move the glyphs, otherwise an otherwise identical
    // frame shifts by a pixel and stops being bit-exact.
    lv_obj_set_width(ui.nav_accel, px(72));
    lv_obj_set_style_text_align(ui.nav_accel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(ui.nav_accel, lv_font_montserrat_20.line_height);
}
// Speed-difference acceleration readout for the speed arc (plan §6.4). The
// pure filter lives in accel_filter.h; this only paints it.
void update_accel_readout(unsigned speed_kph) {
    // The readout is time based, so repaint it at most every 100 ms; painting
    // every frame would make the navigation page depend on the wall clock.
    const std::uint32_t now_ms = lv_tick_get();
    const std::uint32_t last_ms = ui.accel_readout_ms;
    const std::uint32_t elapsed = now_ms >= last_ms ? now_ms - last_ms : 0;
    // Only advance the filter when the speed actually changed. Feeding a
    // constant speed would keep evolving the EMA (time based slope) and make
    // an otherwise identical frame differ, which breaks deterministic
    // renders and shows a phantom acceleration at steady speed.
    if(ui.accel_readout_valid && elapsed < 100 &&
       speed_kph == ui.accel_readout_speed_kph) {
        return;
    }
    ui.accel_readout_ms = now_ms;
    ui.accel_readout_valid = true;
    // A discontinuous speed (first fix, or a jump the window cannot explain)
    // restarts the window: differentiating across the gap would report a huge
    // phantom acceleration while the EMA catches up.
    if(ui.accel_readout_speed_kph != 0xFFFFFFFFu &&
       (speed_kph > ui.accel_readout_speed_kph + 5 ||
        speed_kph + 5 < ui.accel_readout_speed_kph)) {
        moto::nav::ui::accel_filter_reset(ui.accel_filter);
    }
    ui.accel_readout_speed_kph = speed_kph;
    moto::nav::ui::accel_filter_push(ui.accel_filter,
                                     static_cast<float>(speed_kph),
                                     now_ms);
    const float accel_mps2 = ui.accel_filter.accel_mps2;
    lv_color_t color = kIce;
    if(accel_mps2 > 0.5F) {
        color = kGreen;
    } else if(accel_mps2 < -0.5F) {
        color = kRed;
    }
    if(ui.nav_speed_arc_object != nullptr) {
        lv_obj_set_style_arc_color(ui.nav_speed_arc_object, color,
                                   LV_PART_INDICATOR);
    }
    if(ui.nav_accel == nullptr) return;
    // A negative zero must never print as "+-0.0": that would change the label
    // width and shift the readout by a pixel between otherwise equal frames.
    const float shown = accel_mps2 == 0.0F ? 0.0F : accel_mps2;
    if(std::abs(shown) < 0.05F) {
        // A constant-speed frame has no acceleration to report; leaving "+0.0"
        // on screen made the readout look like an unexplained value.
        lv_obj_add_flag(ui.nav_accel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(ui.nav_accel, LV_OBJ_FLAG_HIDDEN);
    char text[12];
    if(shown > 0.0F) {
        std::snprintf(text, sizeof(text), "+%.1f", shown);
    } else {
        std::snprintf(text, sizeof(text), "%.1f", shown);
    }
    lv_label_set_text(ui.nav_accel, text);
    lv_obj_set_style_text_color(ui.nav_accel, color, 0);
}

void update_nav_status(const moto_ui_state_t *state) {
    const char *text = "";
    lv_color_t color = kQuiet;
    if(ui.demo_active) {
        lv_label_set_text(ui.nav_status, "DEMO RIDE");
        lv_obj_set_style_text_color(ui.nav_status, kWhite, 0);
        return;
    }
    if(state->mode != MOTO_UI_ARRIVED && state->route_point_count < 2) {
        // Empty navigation states are rendered by the full lifecycle surface,
        // never by a tiny diagnostic caption floating in a black screen.
        lv_label_set_text(ui.nav_status, "");
        return;
    }
    switch(state->mode) {
        case MOTO_UI_ACQUIRING_FIX: text = "ACQUIRING GPS"; color = kAmber; break;
        case MOTO_UI_REROUTING: text = "REROUTING"; color = kAmber; break;
        case MOTO_UI_OFFLINE: text = "ROUTE CACHED"; color = kQuiet; break;
        case MOTO_UI_ARRIVED: text = "ARRIVED"; color = kGreen; break;
        case MOTO_UI_NAVIGATING:
            if(state->online == 0) text = "NO HOTSPOT";
            break;
    }
    lv_label_set_text(ui.nav_status, text);
    lv_obj_set_style_text_color(ui.nav_status, color, 0);
}

void update_navigation(const moto_ui_state_t *state) {
    update_building_geometry(state);
    update_road_geometry(state);
    update_route_geometry(state);
    update_maneuver(state->maneuver);
    update_nav_status(state);

    // North-up: the map stays fixed and the vehicle marker carries the real
    // phone heading (plan §4.7). Heading-up keeps the marker pointing up
    // because the whole map rotates under it.
    if(ui.nav_marker != nullptr) {
        g_marker_rotation_deg =
            ui.layout.north_up ? static_cast<int>(state->heading_deg) : 0;
        // Repaint the marker through its draw callback; the callback rotates
        // the arrow itself (style transform_rotation rotated the whole page).
        lv_obj_invalidate(ui.nav_marker);
    }

    // Never suggest "go straight for 0 m" while there is no route. Waiting is
    // a connection state, not actionable navigation guidance.
    // Guidance begins as soon as the phone reports a navigating state, even if
    // the (fragmented) route geometry has not reassembled yet. Gating this on
    // route_point_count left the device on READY/请选择目的地 while the phone
    // was already navigating (device-reported).
    const bool has_guidance = ui.demo_active ||
                              state->mode == MOTO_UI_NAVIGATING ||
                              state->mode == MOTO_UI_REROUTING ||
                              state->mode == MOTO_UI_ARRIVED ||
                              state->route_point_count >= 2;
    ui.navigation_has_guidance = has_guidance;
    ui.navigation_route_request_in_flight =
        state->route_request_in_flight != 0;
    set_navigation_guidance_visible(has_guidance);
    refresh_lifecycle();

    // Speed / heading / speed-arc do not depend on a route: they stay live
    // while the phone is acquiring a fix, exactly like the dedicated pages.
    {
        const moto_ui_component_layout_t *speed =
            find_component(ui.layout, MOTO_UI_COMPONENT_SPEED);
        const moto_ui_component_layout_t *heading =
            find_component(ui.layout, MOTO_UI_COMPONENT_HEADING);
        const moto_ui_component_layout_t *speed_arc =
            find_component(ui.layout, MOTO_UI_COMPONENT_SPEED_ARC);
        const moto_ui_component_layout_t *speed_limit =
            find_component(ui.layout, MOTO_UI_COMPONENT_SPEED_LIMIT);

        char speed_text[8];
        std::snprintf(speed_text, sizeof(speed_text), "%u",
                      static_cast<unsigned>(state->speed_kph));
        lv_label_set_text(ui.nav_speed, speed_text);
        lv_obj_align_to(ui.nav_speed_unit, ui.nav_speed,
                        LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

        char heading_text[12];
        std::snprintf(heading_text, sizeof(heading_text), "%03u°",
                      static_cast<unsigned>(state->heading_deg));
        lv_label_set_text(ui.nav_heading, heading_text);
        lv_label_set_text(ui.nav_heading_cardinal,
                          cardinal_name(state->heading_deg));
        lv_obj_align_to(ui.nav_heading_cardinal, ui.nav_heading,
                        LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

        set_component_visible(ui.nav_speed, speed != nullptr && speed->enabled, true);
        set_component_visible(ui.nav_speed_unit, speed != nullptr && speed->enabled,
                              true);
        set_component_visible(ui.nav_heading,
                              heading != nullptr && heading->enabled, true);
        set_component_visible(ui.nav_heading_cardinal,
                              heading != nullptr && heading->enabled, true);
        set_component_visible(ui.nav_speed_arc_object,
                              speed_arc != nullptr && speed_arc->enabled, true);
        // The acceleration readout is intentionally not shown: at a constant
        // speed it rendered as an unexplained "+0.0" in the middle of the map
        // (device-reported). The arc itself still shows speed.

        // Speed limit: hidden when the provider reports zero.
        if(state->speed_limit_kph > 0) {
            char limit[8];
            std::snprintf(limit, sizeof(limit), "%u",
                          static_cast<unsigned>(state->speed_limit_kph));
            lv_label_set_text(ui.nav_limit_value, limit);
            set_component_visible(ui.nav_limit,
                                  speed_limit != nullptr && speed_limit->enabled,
                                  true);
        } else {
            lv_obj_add_flag(ui.nav_limit, LV_OBJ_FLAG_HIDDEN);
        }

        if(speed_arc != nullptr && speed_arc->enabled) {
            lv_arc_set_value(ui.nav_speed_arc_object,
                             std::min<int>(160, static_cast<int>(state->speed_kph)));
        }
    }

    if(!has_guidance) return;

    char value[16];
    const char *unit = "m";
    if(state->distance_to_maneuver_m >= 1000) {
        std::snprintf(value, sizeof(value), "%.1f",
                      state->distance_to_maneuver_m / 1000.0);
        unit = "km";
    } else {
        std::snprintf(value, sizeof(value), "%u",
                      static_cast<unsigned>(state->distance_to_maneuver_m));
    }
    if(state->mode == MOTO_UI_ARRIVED) std::snprintf(value, sizeof(value), "0");
    lv_label_set_text(ui.nav_distance, value);
    lv_label_set_text(ui.nav_unit, unit);
    // The distance text is part of the maneuver component. Hide it when the
    // snapshot carries no next maneuver instead of leaving the constructor's
    // placeholder visible as a mysterious "0.0" in the middle of the screen.
    const bool has_maneuver = state->has_next_maneuver != 0 ||
                              state->mode == MOTO_UI_ARRIVED;
    if(has_maneuver) {
        lv_obj_clear_flag(ui.nav_distance, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui.nav_unit, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.nav_distance, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.nav_unit, LV_OBJ_FLAG_HIDDEN);
    }
    // The distance is anchored inside the maneuver component box (layout
    // position), never by an absolute per-frame align: a layout push moves
    // the box and this text follows on the next frame.
    lv_obj_align_to(ui.nav_distance, ui.nav_maneuver, LV_ALIGN_OUT_RIGHT_MID,
                    px(2), 0);
    lv_obj_align_to(ui.nav_unit, ui.nav_distance, LV_ALIGN_OUT_RIGHT_BOTTOM,
                    px(6), px(-7));

    lv_arc_set_value(ui.nav_progress,
                     std::min<int>(100, state->route_progress_percent));
    lv_obj_set_style_arc_color(ui.nav_progress, traffic_color(state->traffic),
                               LV_PART_INDICATOR);

    // --- Layout components: remaining distance / time (plan §4.2) ---------
    if(ui.nav_remaining_distance != nullptr) {
        char remaining[16];
        if(state->remaining_distance_m >= 1000) {
            std::snprintf(remaining, sizeof(remaining), "%.1f",
                          state->remaining_distance_m / 1000.0);
            lv_label_set_text(ui.nav_remaining_distance_caption, "剩余 km");
        } else {
            std::snprintf(remaining, sizeof(remaining), "%u",
                          static_cast<unsigned>(state->remaining_distance_m));
            lv_label_set_text(ui.nav_remaining_distance_caption, "剩余 m");
        }
        lv_label_set_text(ui.nav_remaining_distance, remaining);
        lv_obj_align_to(ui.nav_remaining_distance_caption,
                        ui.nav_remaining_distance,
                        LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
    if(ui.nav_remaining_time != nullptr) {
        char remaining[16];
        if(state->remaining_time_s >= 3600) {
            std::snprintf(remaining, sizeof(remaining), "%uh%02um",
                          static_cast<unsigned>(state->remaining_time_s / 3600),
                          static_cast<unsigned>(
                              (state->remaining_time_s % 3600) / 60));
            lv_label_set_text(ui.nav_remaining_time_caption, "剩余时间");
        } else {
            std::snprintf(remaining, sizeof(remaining), "%u",
                          static_cast<unsigned>(state->remaining_time_s / 60));
            lv_label_set_text(ui.nav_remaining_time_caption, "剩余分钟");
        }
        lv_label_set_text(ui.nav_remaining_time, remaining);
        lv_obj_align_to(ui.nav_remaining_time_caption, ui.nav_remaining_time,
                        LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
}

void update_speedometer(const moto_ui_state_t *state) {
    char value[8];
    std::snprintf(value, sizeof(value), "%u",
                  static_cast<unsigned>(state->speed_kph));
    lv_label_set_text(ui.speed_value, value);
    lv_obj_align(ui.speed_value, LV_ALIGN_CENTER, 0, px(-10));
    lv_arc_set_value(ui.speed_arc, std::min<int>(160, state->speed_kph));
    for(int i = 0; i < kSpeedTickCount; ++i) {
        const bool active = state->speed_kph >= static_cast<unsigned>(i * 10);
        lv_obj_set_style_line_color(ui.speed_ticks[i], active ? kWhite : kGraphite, 0);
    }
}

void update_compass(const moto_ui_state_t *state) {
    char heading[12];
    std::snprintf(heading, sizeof(heading), "%03u°",
                  static_cast<unsigned>(state->heading_deg));
    lv_label_set_text(ui.compass_heading, heading);
    lv_label_set_text(ui.compass_cardinal, cardinal_name(state->heading_deg));
    char speed[20];
    std::snprintf(speed, sizeof(speed), "%u km/h",
                  static_cast<unsigned>(state->speed_kph));
    lv_label_set_text(ui.compass_speed, speed);

    const double heading_rad = state->heading_deg * kPi / 180.0;
    for(int i = 0; i < kCompassTickCount; ++i) {
        const double angle = i * 15.0 * kPi / 180.0 - heading_rad;
        const double inner = px(i % 3 == 0 ? 138.0 : 145.0);
        ui.compass_tick_points[i][0] = {
            static_cast<lv_value_precise_t>(px(180.0) + std::sin(angle) * px(154.0)),
            static_cast<lv_value_precise_t>(px(180.0) - std::cos(angle) * px(154.0)),
        };
        ui.compass_tick_points[i][1] = {
            static_cast<lv_value_precise_t>(px(180.0) + std::sin(angle) * inner),
            static_cast<lv_value_precise_t>(px(180.0) - std::cos(angle) * inner),
        };
        lv_line_set_points_mutable(ui.compass_ticks[i],
                                   ui.compass_tick_points[i], 2);
        lv_obj_set_style_line_color(ui.compass_ticks[i], i == 0 ? kAmber : kQuiet, 0);
    }

    static const double bearings[4] = {0.0, 90.0, 180.0, 270.0};
    static const char *letters[4] = {"北", "东", "南", "西"};
    for(int i = 0; i < 4; ++i) {
        const double angle = bearings[i] * kPi / 180.0 - heading_rad;
        const int x = static_cast<int>(px(180.0) + std::sin(angle) * px(112.0));
        const int y = static_cast<int>(px(180.0) - std::cos(angle) * px(112.0));
        lv_label_set_text(ui.compass_letters[i], letters[i]);
        lv_obj_set_pos(ui.compass_letters[i], x - px(14), y - px(10));
        lv_obj_set_style_text_color(ui.compass_letters[i], i == 0 ? kAmber : kQuiet, 0);
    }
}

void update_music_view() {
    lv_label_set_text(ui.music_source, ui.music.connected ? ui.music_source_text : "手机未连接");
    lv_label_set_text(ui.music_title, ui.music_title_text);
    lv_label_set_text(ui.music_artist, ui.music_artist_text);
    lv_label_set_text(ui.music_button_labels[1], ui.music.playing ? "II" : ">");
    lv_label_set_text(ui.music_button_labels[3], ui.music.liked ? "已送" : "喜欢");
    if(ui.music.like_available) {
        lv_obj_remove_flag(ui.music_buttons[3], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.music_buttons[3], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_border_color(ui.music_disc,
                                  ui.music.playing ? kIce : kGraphite, 0);
}

void music_button_event(lv_event_t *event) {
    const auto command = static_cast<moto_music_command_t>(
        reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event)));
    if(command == MOTO_MUSIC_TOGGLE_PLAYBACK) {
        ui.music.playing = ui.music.playing == 0;
    } else if(command == MOTO_MUSIC_LIKE && ui.music.like_available) {
        ui.music.liked = 1;
    }
    update_music_view();
    if(ui.music_callback != nullptr) ui.music_callback(command, ui.music_callback_context);
}

void create_page_dots() {
    for(int i = 0; i < MOTO_UI_PAGE_COUNT; ++i) {
        ui.page_dots[i] = lv_obj_create(ui.screen);
        lv_obj_remove_style_all(ui.page_dots[i]);
        lv_obj_set_size(ui.page_dots[i], px(5), px(5));
        lv_obj_set_y(ui.page_dots[i], px(337));
        lv_obj_set_style_bg_opa(ui.page_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ui.page_dots[i], px(3), 0);
        lv_obj_clear_flag(ui.page_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
    ui.page_dots_timer = lv_timer_create(hide_page_dots,
                                         kPageDotsVisibleMs, nullptr);
    update_page_dots();
    reveal_page_dots();
}

void create_navigation_page() {
    lv_obj_t *page = ui.pages[MOTO_UI_PAGE_NAVIGATION];
    ui.nav_map = make_layer(page);
    // The map is the topmost hit target across most of the navigation page.
    // Forward swipe gestures to the page/screen. Local demo activation is not
    // bound to a long press: a glove, mount or palm must never replace active
    // phone guidance with the built-in fixture while riding.
    lv_obj_add_flag(ui.nav_map, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(ui.nav_map, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_height(ui.nav_map, px(232));

    // Two sibling layers instead of one: the basemap (roads + buildings) and
    // the selected route are dimmed independently by the layout, so a
    // translucent basemap never fades the route with it.
    ui.nav_map_base = make_layer(ui.nav_map);
    lv_obj_set_height(ui.nav_map_base, px(232));
    ui.nav_route_layer = make_layer(ui.nav_map);
    lv_obj_set_height(ui.nav_route_layer, px(232));

    // Real building footprints are quiet closed outlines beneath both roads
    // and the selected route. No inferred rectangles or decorative fill is
    // generated on-device.
    for(std::uint8_t i = 0;
        i < MOTO_UI_BUILDING_FOOTPRINT_CAPACITY; ++i) {
        ui.nav_buildings[i] = create_map_polyline(ui.nav_map_base,
                                                  ui.nav_building_lines[i]);
        lv_obj_set_size(ui.nav_buildings[i], MOTO_UI_CANVAS_WIDTH, px(232));
        lv_obj_set_style_line_width(ui.nav_buildings[i], px(1), 0);
        lv_obj_set_style_line_color(ui.nav_buildings[i], kBuildingGray, 0);
        lv_obj_set_style_line_opa(ui.nav_buildings[i], LV_OPA_COVER, 0);
        lv_obj_set_style_line_rounded(ui.nav_buildings[i], false, 0);
        lv_obj_add_flag(ui.nav_buildings[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Quiet, real street context sits behind the selected route. Each road is
    // its own LVGL line so disconnected streets are never joined by a fake
    // diagonal. The bundled Jinan fixture uses all eight bounded slots; a
    // future online provider must simplify its response to the same limit.
    for(std::uint8_t i = 0; i < MOTO_UI_ROAD_POLYLINE_CAPACITY; ++i) {
        ui.nav_roads[i] = create_map_polyline(ui.nav_map_base, ui.nav_road_lines[i]);
        lv_obj_set_size(ui.nav_roads[i], MOTO_UI_CANVAS_WIDTH, px(232));
        lv_obj_set_style_line_width(ui.nav_roads[i], px(3), 0);
        lv_obj_set_style_line_color(ui.nav_roads[i], kRoadGray, 0);
        lv_obj_set_style_line_opa(ui.nav_roads[i], LV_OPA_COVER, 0);
        lv_obj_set_style_line_rounded(ui.nav_roads[i], true, 0);
        lv_obj_add_flag(ui.nav_roads[i], LV_OBJ_FLAG_HIDDEN);
    }

    ui.nav_route_shadow = create_map_polyline(ui.nav_route_layer, ui.nav_route_shadow_line);
    lv_obj_set_size(ui.nav_route_shadow, MOTO_UI_CANVAS_WIDTH, px(232));
    lv_obj_set_style_line_width(ui.nav_route_shadow, px(13), 0);
    lv_obj_set_style_line_color(ui.nav_route_shadow, kGraphite, 0);
    lv_obj_set_style_line_rounded(ui.nav_route_shadow, true, 0);
    ui.nav_route = create_map_polyline(ui.nav_route_layer, ui.nav_route_line);
    lv_obj_set_size(ui.nav_route, MOTO_UI_CANVAS_WIDTH, px(232));
    lv_obj_set_style_line_width(ui.nav_route, px(6), 0);
    lv_obj_set_style_line_color(ui.nav_route, kWhite, 0);
    lv_obj_set_style_line_rounded(ui.nav_route, true, 0);
    ui.nav_route_motion_timer = lv_timer_create(route_motion_tick,
                                                 kRouteMotionFrameMs,
                                                 nullptr);

    // The marker is its own layer, not a child of the basemap or the route
    // layer: neither map_opacity nor route_opacity may dim the rider (§6.2).
    ui.nav_marker = lv_obj_create(ui.nav_map);
    lv_obj_remove_style_all(ui.nav_marker);
    // Oversized square so a rotated arrow (radius ~26 design px) never clips.
    // Exact y is re-set in apply_layout_internal() from the map height.
    lv_obj_set_size(ui.nav_marker, px(54), px(54));
    lv_obj_set_pos(ui.nav_marker, MOTO_UI_CANVAS_WIDTH / 2 - px(27),
                   MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT / 2 - px(27));
    lv_obj_clear_flag(ui.nav_marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.nav_marker, draw_vehicle_marker,
                        LV_EVENT_DRAW_MAIN_END, nullptr);

    ui.nav_status = make_label(page, &lv_font_montserrat_16, kAmber, "");
    lv_obj_set_width(ui.nav_status, px(220));
    lv_obj_set_style_text_align(ui.nav_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.nav_status, LV_ALIGN_TOP_MID, 0, px(16));

    // The next action is the navigation page's single visual hero. The short
    // road window above is deliberately quieter and contains no fake side
    // streets; this standard symbol must be understood in one glance.
    ui.nav_maneuver = lv_obj_create(page);
    lv_obj_remove_style_all(ui.nav_maneuver);
    lv_obj_set_size(ui.nav_maneuver, px(116), px(108));
    lv_obj_set_pos(ui.nav_maneuver, px(29), px(220));
    lv_obj_clear_flag(ui.nav_maneuver, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.nav_maneuver, draw_maneuver_icon,
                        LV_EVENT_DRAW_MAIN_END, nullptr);

    // The maneuver distance is part of the maneuver component (§C), so it is a
    // child of that container and positioned relatively to the icon.
    ui.nav_distance = make_label(ui.nav_maneuver, &lv_font_montserrat_48,
                                 kWhite, "");
    ui.nav_unit = make_label(ui.nav_maneuver, &lv_font_montserrat_20, kQuiet,
                             "m");

    ui.nav_limit = lv_obj_create(page);
    lv_obj_set_size(ui.nav_limit, px(61), px(61));
    // Keep the optional demo/SDK-provided value inside the circular safe area.
    // Real AMap Web-Service snapshots use zero and remain hidden because that
    // API does not return a road speed limit.
    lv_obj_set_pos(ui.nav_limit, px(248), px(238));
    lv_obj_set_style_radius(ui.nav_limit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui.nav_limit, kWhite, 0);
    lv_obj_set_style_bg_opa(ui.nav_limit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ui.nav_limit, kRed, 0);
    lv_obj_set_style_border_width(ui.nav_limit, px(6), 0);
    lv_obj_set_style_pad_all(ui.nav_limit, 0, 0);
    lv_obj_clear_flag(ui.nav_limit, LV_OBJ_FLAG_CLICKABLE);
    ui.nav_limit_value = make_label(ui.nav_limit, &lv_font_montserrat_28, kBlack, "70");
    lv_obj_center(ui.nav_limit_value);

    // --- Layout-editable components (plan §4.2) ---------------------------
    // Created once here; apply_layout() repositions/resizes/restyles them.
    ui.nav_speed = make_label(page, &lv_font_montserrat_48, kWhite, "72");
    ui.nav_speed_unit = make_label(page, &lv_font_montserrat_20, kQuiet, "km/h");
    lv_obj_add_flag(ui.nav_speed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.nav_speed_unit, LV_OBJ_FLAG_HIDDEN);

    ui.nav_heading = make_label(page, &lv_font_montserrat_48, kWhite, "0");
    ui.nav_heading_cardinal =
        make_label(page, &moto_font_nav_16, kQuiet, "N");
    lv_obj_add_flag(ui.nav_heading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.nav_heading_cardinal, LV_OBJ_FLAG_HIDDEN);

    ui.nav_remaining_distance =
        make_label(page, &lv_font_montserrat_48, kWhite, "");
    ui.nav_remaining_distance_caption =
        make_label(page, &moto_font_nav_16, kQuiet, "剩余");
    lv_obj_add_flag(ui.nav_remaining_distance, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.nav_remaining_distance_caption, LV_OBJ_FLAG_HIDDEN);

    ui.nav_remaining_time =
        make_label(page, &lv_font_montserrat_48, kWhite, "");
    ui.nav_remaining_time_caption =
        make_label(page, &moto_font_nav_16, kQuiet, "剩余时间");
    lv_obj_add_flag(ui.nav_remaining_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.nav_remaining_time_caption, LV_OBJ_FLAG_HIDDEN);

    // Signed acceleration readout pinned inside the speed arc (plan §6.4).
    ui.nav_accel = make_label(page, &moto_font_nav_16, kIce, "+0.0");
    lv_obj_add_flag(ui.nav_accel, LV_OBJ_FLAG_HIDDEN);

    // Speed/acceleration arc (§4.5): hugs the bezel like the progress arc and
    // is recoloured by the acceleration sign.
    ui.nav_speed_arc_object = lv_arc_create(page);
    lv_arc_set_range(ui.nav_speed_arc_object, 0, 160);
    lv_obj_remove_style(ui.nav_speed_arc_object, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(ui.nav_speed_arc_object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(ui.nav_speed_arc_object, kGraphite, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.nav_speed_arc_object, kIce, LV_PART_INDICATOR);
    lv_obj_add_flag(ui.nav_speed_arc_object, LV_OBJ_FLAG_HIDDEN);

    ui.nav_progress = lv_arc_create(page);
    lv_obj_set_size(ui.nav_progress, px(316), px(316));
    lv_obj_center(ui.nav_progress);
    lv_arc_set_rotation(ui.nav_progress, 48);
    lv_arc_set_bg_angles(ui.nav_progress, 0, 84);
    lv_arc_set_range(ui.nav_progress, 0, 100);
    lv_arc_set_value(ui.nav_progress, 24);
    lv_obj_remove_style(ui.nav_progress, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(ui.nav_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ui.nav_progress, px(3), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.nav_progress, kGraphite, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.nav_progress, px(4), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.nav_progress, kIce, LV_PART_INDICATOR);
    lv_obj_move_to_index(ui.nav_progress, 0);

    // Connection lifecycle overlay. The composition follows the same circular
    // safe area as the navigation instrument and intentionally uses one hero
    // symbol instead of a dashboard of technical status labels.
    ui.nav_lifecycle = make_layer(page);
    lv_obj_set_style_bg_color(ui.nav_lifecycle, kBlack, 0);
    lv_obj_set_style_bg_opa(ui.nav_lifecycle, LV_OPA_COVER, 0);
    lv_obj_add_flag(ui.nav_lifecycle, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(ui.nav_lifecycle, LV_OBJ_FLAG_GESTURE_BUBBLE);

    ui.nav_lifecycle_kicker = make_label(
        ui.nav_lifecycle, &moto_font_nav_16, kQuiet,
        "MOTO GPS / 手机连接");
    lv_obj_set_style_text_letter_space(ui.nav_lifecycle_kicker, px(2), 0);
    lv_obj_align(ui.nav_lifecycle_kicker, LV_ALIGN_TOP_MID, 0, px(39));

    ui.nav_lifecycle_symbol = lv_obj_create(ui.nav_lifecycle);
    lv_obj_remove_style_all(ui.nav_lifecycle_symbol);
    lv_obj_set_size(ui.nav_lifecycle_symbol, px(150), px(150));
    lv_obj_align(ui.nav_lifecycle_symbol, LV_ALIGN_TOP_MID, 0, px(61));
    lv_obj_clear_flag(ui.nav_lifecycle_symbol, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.nav_lifecycle_symbol, draw_connection_symbol,
                        LV_EVENT_DRAW_MAIN_END, nullptr);

    ui.nav_lifecycle_title = make_label(
        ui.nav_lifecycle, &moto_font_nav_28, kWhite, "连接手机");
    lv_obj_set_width(ui.nav_lifecycle_title, px(310));
    lv_obj_set_style_text_align(ui.nav_lifecycle_title,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(ui.nav_lifecycle_title, px(2), 0);
    lv_obj_align(ui.nav_lifecycle_title, LV_ALIGN_TOP_MID, 0, px(231));

    ui.nav_lifecycle_subtitle = make_label(
        ui.nav_lifecycle, &moto_font_nav_16, kSoft, "请打开手机应用");
    lv_obj_set_width(ui.nav_lifecycle_subtitle, px(280));
    lv_obj_set_style_text_align(ui.nav_lifecycle_subtitle,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.nav_lifecycle_subtitle, LV_ALIGN_TOP_MID, 0, px(278));

    ui.nav_lifecycle_timer = lv_timer_create(lifecycle_tick, 40, nullptr);
    ui.nav_success_timer = lv_timer_create(connection_success_timeout,
                                            kConnectionSuccessHoldMs,
                                            nullptr);
    lv_timer_pause(ui.nav_success_timer);
    apply_lifecycle_content(LifecycleVisual::PhoneOffline);
    ui.lifecycle_target = LifecycleVisual::PhoneOffline;
}

void create_speed_page() {
    lv_obj_t *page = ui.pages[MOTO_UI_PAGE_SPEED];
    lv_obj_t *title = make_label(page, &lv_font_montserrat_16, kQuiet, "SPEED");
    lv_obj_set_style_text_letter_space(title, px(4), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, px(48));

    ui.speed_arc = lv_arc_create(page);
    lv_obj_set_size(ui.speed_arc, px(312), px(312));
    lv_obj_center(ui.speed_arc);
    lv_arc_set_rotation(ui.speed_arc, 138);
    lv_arc_set_bg_angles(ui.speed_arc, 0, 264);
    lv_arc_set_range(ui.speed_arc, 0, 160);
    lv_obj_remove_style(ui.speed_arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(ui.speed_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ui.speed_arc, px(5), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.speed_arc, kGraphite, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.speed_arc, px(7), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.speed_arc, kIce, LV_PART_INDICATOR);

    for(int i = 0; i < kSpeedTickCount; ++i) {
        const double angle = (138.0 + i * (264.0 / (kSpeedTickCount - 1))) * kPi / 180.0;
        const double inner = px(i % 2 == 0 ? 132.0 : 136.0);
        ui.speed_tick_points[i][0] = {
            static_cast<lv_value_precise_t>(px(180.0) + std::cos(angle) * px(142.0)),
            static_cast<lv_value_precise_t>(px(180.0) + std::sin(angle) * px(142.0)),
        };
        ui.speed_tick_points[i][1] = {
            static_cast<lv_value_precise_t>(px(180.0) + std::cos(angle) * inner),
            static_cast<lv_value_precise_t>(px(180.0) + std::sin(angle) * inner),
        };
        ui.speed_ticks[i] = lv_line_create(page);
        lv_obj_set_size(ui.speed_ticks[i], MOTO_UI_CANVAS_WIDTH,
                        MOTO_UI_CANVAS_HEIGHT);
        lv_line_set_points_mutable(ui.speed_ticks[i], ui.speed_tick_points[i], 2);
        lv_obj_set_style_line_width(ui.speed_ticks[i], px(i % 2 == 0 ? 3 : 2), 0);
        lv_obj_set_style_line_color(ui.speed_ticks[i], kGraphite, 0);
    }
    ui.speed_value = make_label(page, &lv_font_montserrat_48, kWhite, "72");
    lv_obj_align(ui.speed_value, LV_ALIGN_CENTER, 0, px(-10));
    lv_obj_t *unit = make_label(page, &lv_font_montserrat_20, kQuiet, "km/h");
    lv_obj_set_style_text_letter_space(unit, px(2), 0);
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, px(45));
    lv_obj_t *caption = make_label(page, &lv_font_montserrat_16, kQuiet, "LIVE SPEED");
    lv_obj_set_style_text_letter_space(caption, px(2), 0);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, px(-49));
}

void create_compass_page() {
    lv_obj_t *page = ui.pages[MOTO_UI_PAGE_COMPASS];
    for(int i = 0; i < kCompassTickCount; ++i) {
        ui.compass_ticks[i] = lv_line_create(page);
        lv_obj_set_size(ui.compass_ticks[i], MOTO_UI_CANVAS_WIDTH,
                        MOTO_UI_CANVAS_HEIGHT);
        lv_obj_set_style_line_width(ui.compass_ticks[i], px(i % 3 == 0 ? 3 : 2), 0);
        lv_obj_set_style_line_color(ui.compass_ticks[i], kQuiet, 0);
    }
    for(int i = 0; i < 4; ++i) {
        ui.compass_letters[i] = make_label(page, &moto_font_nav_16, kQuiet, "北");
        lv_obj_set_size(ui.compass_letters[i], px(28), px(22));
        lv_obj_set_style_text_align(ui.compass_letters[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *index = lv_obj_create(page);
    lv_obj_remove_style_all(index);
    lv_obj_set_size(index, px(9), px(9));
    lv_obj_set_pos(index, px(176), px(18));
    lv_obj_set_style_bg_color(index, kRed, 0);
    lv_obj_set_style_bg_opa(index, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_rotation(index, 450, 0);

    ui.compass_heading = make_label(page, &lv_font_montserrat_48, kAmber, "359°");
    lv_obj_align(ui.compass_heading, LV_ALIGN_CENTER, 0, px(-37));
    ui.compass_cardinal = make_label(page, &lv_font_montserrat_20, kAmber, "N");
    lv_obj_align(ui.compass_cardinal, LV_ALIGN_CENTER, 0, px(10));
    ui.compass_speed = make_label(page, &lv_font_montserrat_20, kWhite, "72 km/h");
    lv_obj_set_style_text_letter_space(ui.compass_speed, px(1), 0);
    lv_obj_align(ui.compass_speed, LV_ALIGN_CENTER, 0, px(57));
}

void create_music_page() {
    lv_obj_t *page = ui.pages[MOTO_UI_PAGE_MUSIC];
    ui.music_source = make_label(page, &moto_font_nav_16, kQuiet, "手机音乐");
    lv_obj_set_style_text_letter_space(ui.music_source, px(2), 0);
    lv_obj_align(ui.music_source, LV_ALIGN_TOP_MID, 0, px(37));

    ui.music_disc = lv_obj_create(page);
    lv_obj_set_size(ui.music_disc, px(104), px(104));
    lv_obj_align(ui.music_disc, LV_ALIGN_TOP_MID, 0, px(71));
    lv_obj_set_style_radius(ui.music_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui.music_disc, kGraphite, 0);
    lv_obj_set_style_bg_opa(ui.music_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.music_disc, px(5), 0);
    lv_obj_set_style_border_color(ui.music_disc, kIce, 0);
    lv_obj_set_style_pad_all(ui.music_disc, 0, 0);
    lv_obj_t *core = lv_obj_create(ui.music_disc);
    lv_obj_set_size(core, px(30), px(30));
    lv_obj_center(core);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(core, kBlack, 0);
    lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(core, 0, 0);

    ui.music_title = make_label(page, &moto_font_nav_20, kWhite, "未在播放");
    lv_obj_set_width(ui.music_title, px(260));
    lv_obj_set_style_text_align(ui.music_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.music_title, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.music_title, LV_ALIGN_TOP_MID, 0, px(190));
    ui.music_artist = make_label(page, &moto_font_nav_16, kQuiet, "手机媒体");
    lv_obj_set_width(ui.music_artist, px(260));
    lv_obj_set_style_text_align(ui.music_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.music_artist, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.music_artist, LV_ALIGN_TOP_MID, 0, px(216));

    static const char *labels[4] = {"<<", ">", ">>", "喜欢"};
    static const int positions[4][3] = {
        {72, 254, 54}, {153, 247, 62}, {234, 254, 54}, {147, 312, 66},
    };
    for(int i = 0; i < 4; ++i) {
        ui.music_buttons[i] = lv_button_create(page);
        lv_obj_set_size(ui.music_buttons[i], px(positions[i][2]),
                        px(i == 3 ? 28 : positions[i][2]));
        lv_obj_set_pos(ui.music_buttons[i], px(positions[i][0]),
                       px(positions[i][1]));
        lv_obj_set_style_radius(ui.music_buttons[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ui.music_buttons[i], i == 1 ? kWhite : kGraphite, 0);
        lv_obj_set_style_bg_opa(ui.music_buttons[i], LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(ui.music_buttons[i], 0, 0);
        lv_obj_set_style_border_width(ui.music_buttons[i], 0, 0);
        // Pin the pressed appearance to the idle appearance. The LVGL default
        // theme darkens and grows pressed buttons (recolor 35% + 3 px grow);
        // on the ESP32 PPA render path that pre-composited recolor painted as
        // a hard-edged yellow-green block over the round button. Identical
        // pressed styles remove the visual state difference by construction.
        lv_obj_set_style_bg_color(ui.music_buttons[i], i == 1 ? kWhite : kGraphite,
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(ui.music_buttons[i], LV_OPA_COVER,
                                LV_STATE_PRESSED);
        lv_obj_set_style_radius(ui.music_buttons[i], LV_RADIUS_CIRCLE,
                                LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(ui.music_buttons[i], 0, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(ui.music_buttons[i], 0, LV_STATE_PRESSED);
        lv_obj_set_style_recolor_opa(ui.music_buttons[i], LV_OPA_TRANSP,
                                     LV_STATE_PRESSED);
        lv_obj_set_style_transform_width(ui.music_buttons[i], 0,
                                         LV_STATE_PRESSED);
        lv_obj_set_style_transform_height(ui.music_buttons[i], 0,
                                          LV_STATE_PRESSED);
        lv_obj_add_flag(ui.music_buttons[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(
            ui.music_buttons[i], music_button_event, LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        ui.music_button_labels[i] = make_label(
            ui.music_buttons[i], i == 3 ? &moto_font_nav_16 : &lv_font_montserrat_20,
            i == 1 ? kBlack : kWhite, labels[i]);
        lv_obj_center(ui.music_button_labels[i]);
    }
}

void set_boot_content_opacity(void *object, int32_t opacity) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), opacity, 0);
}

}  // namespace

extern "C" void moto_nav_ui_show_boot_screen(void) {
    /* This entry point is also used while the full UI is live. Delete every
       timer that retains widget pointers before reset_ui_state() and
       lv_obj_clean() invalidate the tree. */
    if(ui.page_dots_timer != nullptr) {
        lv_timer_delete(ui.page_dots_timer);
        ui.page_dots_timer = nullptr;
    }
    if(ui.nav_route_motion_timer != nullptr) {
        lv_timer_delete(ui.nav_route_motion_timer);
        ui.nav_route_motion_timer = nullptr;
    }
    if(ui.nav_lifecycle_timer != nullptr) {
        lv_timer_delete(ui.nav_lifecycle_timer);
        ui.nav_lifecycle_timer = nullptr;
    }
    if(ui.nav_success_timer != nullptr) {
        lv_timer_delete(ui.nav_success_timer);
        ui.nav_success_timer = nullptr;
    }
    reset_ui_state();
    ui.screen = lv_screen_active();
    lv_obj_clean(ui.screen);
    lv_obj_remove_flag(ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    const lv_color_t boot_black = LV_COLOR_MAKE(0x00, 0x00, 0x00);
    const lv_color_t boot_white = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
    lv_obj_set_style_bg_color(ui.screen, boot_black, 0);
    lv_obj_set_style_bg_opa(ui.screen, LV_OPA_COVER, 0);

    // Keep the AMOLED background truly black and animate only the white mark.
    // A single opacity animation with a delayed reverse is cheaper than
    // per-letter animation and fits inside the existing 1.25 s boot cadence.
    lv_obj_t *content = make_layer(ui.screen);
    lv_obj_set_style_opa(content, LV_OPA_TRANSP, 0);

    lv_obj_t *moto = make_label(content, &lv_font_montserrat_48,
                                boot_white, "MOTO");
    lv_obj_set_style_text_letter_space(moto, px(4), 0);
    lv_obj_set_style_text_outline_stroke_color(moto, boot_white, 0);
    lv_obj_set_style_text_outline_stroke_width(moto, px(2), 0);
    lv_obj_set_style_text_outline_stroke_opa(moto, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale_x(moto, 340, 0);
    lv_obj_set_style_transform_scale_y(moto, 340, 0);
    lv_obj_align(moto, LV_ALIGN_CENTER, 0, px(-37));

    lv_obj_t *gps = make_label(content, &lv_font_montserrat_48,
                               boot_white, "GPS");
    lv_obj_set_style_text_letter_space(gps, px(10), 0);
    lv_obj_set_style_text_outline_stroke_color(gps, boot_white, 0);
    lv_obj_set_style_text_outline_stroke_width(gps, px(2), 0);
    lv_obj_set_style_text_outline_stroke_opa(gps, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale_x(gps, 340, 0);
    lv_obj_set_style_transform_scale_y(gps, 340, 0);
    lv_obj_align(gps, LV_ALIGN_CENTER, 0, px(35));

    // One deliberate signature: a road-like diagonal cut through two solid
    // white velocity bars. No grey, colour or decorative loading spinner.
    for(int i = 0; i < 2; ++i) {
        lv_obj_t *bar = lv_obj_create(content);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, px(i == 0 ? 56 : 38), px(5));
        lv_obj_set_pos(bar, px(i == 0 ? 73 : 250), px(i == 0 ? 259 : 94));
        lv_obj_set_style_bg_color(bar, boot_white, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_transform_rotation(bar, -120, 0);
    }

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, content);
    lv_anim_set_exec_cb(&fade, set_boot_content_opacity);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_delay(&fade, 30);
    lv_anim_set_duration(&fade, 230);
    lv_anim_set_reverse_delay(&fade, 650);
    lv_anim_set_reverse_duration(&fade, 260);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in_out);
    lv_anim_start(&fade);
}

extern "C" void moto_nav_ui_show_power_off_screen(void) {
    if(ui.screen == nullptr) return;
    if(ui.page_dots_timer != nullptr) {
        lv_timer_delete(ui.page_dots_timer);
        ui.page_dots_timer = nullptr;
    }
    if(ui.nav_route_motion_timer != nullptr) {
        lv_timer_delete(ui.nav_route_motion_timer);
        ui.nav_route_motion_timer = nullptr;
    }
    if(ui.nav_lifecycle_timer != nullptr) {
        lv_timer_delete(ui.nav_lifecycle_timer);
        ui.nav_lifecycle_timer = nullptr;
    }
    if(ui.nav_success_timer != nullptr) {
        lv_timer_delete(ui.nav_success_timer);
        ui.nav_success_timer = nullptr;
    }

    lv_obj_clean(ui.screen);
    lv_obj_set_style_bg_color(ui.screen, LV_COLOR_MAKE(0x00, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(ui.screen, LV_OPA_COVER, 0);

    lv_obj_t *brand = make_label(ui.screen, &lv_font_montserrat_48,
                                 LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), "MOTO");
    lv_obj_set_style_text_letter_space(brand, px(4), 0);
    lv_obj_set_style_text_outline_stroke_color(
        brand, LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), 0);
    lv_obj_set_style_text_outline_stroke_width(brand, px(2), 0);
    lv_obj_set_style_text_outline_stroke_opa(brand, LV_OPA_COVER, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, px(-18));

    lv_obj_t *status = make_label(ui.screen, &lv_font_montserrat_16,
                                  LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
                                  "POWER OFF");
    lv_obj_set_style_text_letter_space(status, px(4), 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, px(35));
}

extern "C" void moto_nav_ui_create(void) {
    navui_trace("create: enter");
    reset_ui_state();
    ui.screen = lv_screen_active();
    lv_obj_clean(ui.screen);
    lv_obj_remove_flag(ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui.screen, kBlack, 0);
    lv_obj_set_style_bg_opa(ui.screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(ui.screen, gesture_event, LV_EVENT_GESTURE, nullptr);
    for(int i = 0; i < MOTO_UI_PAGE_COUNT; ++i) {
        ui.pages[i] = make_layer(ui.screen);
        lv_obj_add_flag(ui.pages[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    create_navigation_page();
    create_speed_page();
    create_compass_page();
    create_music_page();
    create_page_dots();
    // Apply the retained layout (default, or the one pushed before create).
    apply_layout_internal(&ui.layout);
    // Press events are delivered to the topmost object under the finger, not
    // necessarily to its page. Register once across the finished tree so any
    // touch reliably wakes the dots, including the map and music controls.
    install_interaction_wake(ui.screen);
    navui_trace("create: before apply_layout");

    moto_music_state_t initial_music = {
        "APPLE MUSIC", "NIGHT RIDE", "PHONE NOW PLAYING", 1, 1, 0, 0,
    };
    moto_nav_ui_set_music_state(&initial_music);

    moto_ui_state_t initial{};
    initial.mode = MOTO_UI_ACQUIRING_FIX;
    initial.page = MOTO_UI_PAGE_NAVIGATION;
    initial.maneuver = MOTO_MANEUVER_STRAIGHT;
    initial.traffic = MOTO_TRAFFIC_UNKNOWN;
    moto_nav_ui_set_state(&initial);
    navui_trace("create: apply done");
    show_page(MOTO_UI_PAGE_NAVIGATION, true);
}

extern "C" void moto_nav_ui_set_state(const moto_ui_state_t *state) {
    if(state == nullptr || ui.screen == nullptr) return;
    // The page is owned by the on-device gesture handler (gesture_event calls
    // show_page directly). Phone snapshots echo display_page back with BLE
    // latency; applying it here reverted the freshly switched page for one
    // frame on every swipe — the visible "jump back to the old page".
    // Boot-time and test entry points still use moto_nav_ui_set_page().
    switch(ui.page) {
        case MOTO_UI_PAGE_NAVIGATION: update_navigation(state); break;
        case MOTO_UI_PAGE_SPEED: update_speedometer(state); break;
        case MOTO_UI_PAGE_COMPASS: update_compass(state); break;
        case MOTO_UI_PAGE_MUSIC:
        case MOTO_UI_PAGE_COUNT: break;
    }
}

extern "C" void moto_nav_ui_set_motion_state(const moto_ui_state_t *state) {
    if(state == nullptr || ui.screen == nullptr || state->page != ui.page) return;
    // QMI8658 samples arrive at high frequency. Only the route polyline or
    // compass rose actually changes with yaw; labels, arcs and hidden pages
    // remain untouched so LVGL submits a small, bounded dirty region.
    if(ui.page == MOTO_UI_PAGE_NAVIGATION) {
        update_building_geometry(state);
        update_road_geometry(state);
        update_route_geometry(state);
    } else if(ui.page == MOTO_UI_PAGE_COMPASS) {
        update_compass(state);
    }
}

extern "C" void moto_nav_ui_set_phone_connection(
    moto_ui_phone_connection_t connection) {
    if(ui.screen == nullptr || connection < MOTO_UI_PHONE_OFFLINE ||
       connection > MOTO_UI_PHONE_ONLINE) {
        return;
    }
    if(ui.phone_connection == connection) return;
    const bool became_online =
        connection == MOTO_UI_PHONE_ONLINE &&
        ui.phone_connection != MOTO_UI_PHONE_ONLINE;
    ui.phone_connection = connection;
    if(became_online) {
        ui.lifecycle_success_active = true;
        if(ui.nav_success_timer != nullptr) {
            lv_timer_set_period(ui.nav_success_timer,
                                kConnectionSuccessHoldMs);
            lv_timer_reset(ui.nav_success_timer);
            lv_timer_resume(ui.nav_success_timer);
        }
    } else if(connection != MOTO_UI_PHONE_ONLINE) {
        ui.lifecycle_success_active = false;
        if(ui.nav_success_timer != nullptr) {
            lv_timer_pause(ui.nav_success_timer);
        }
    }
    refresh_lifecycle();
}

extern "C" void moto_nav_ui_set_reduce_motion(uint8_t reduce_motion) {
    ui.reduce_motion = reduce_motion != 0;
    if(ui.nav_lifecycle_timer != nullptr) {
        if(ui.reduce_motion) {
            lv_timer_pause(ui.nav_lifecycle_timer);
        } else {
            lv_timer_resume(ui.nav_lifecycle_timer);
        }
    }
    refresh_lifecycle();
}

extern "C" void moto_nav_ui_set_page(moto_ui_page_t page) {
    if(ui.screen != nullptr) show_page(page, true);
}

extern "C" moto_ui_page_t moto_nav_ui_get_page(void) {
    return ui.page;
}

extern "C" void moto_nav_ui_set_page_change_callback(
    moto_page_change_callback_t callback, void *context) {
    ui.page_callback = callback;
    ui.page_callback_context = context;
}

extern "C" void moto_nav_ui_set_music_state(const moto_music_state_t *state) {
    if(state == nullptr || ui.screen == nullptr) return;
    ui.music = *state;
    copy_text(ui.music_source_text, sizeof(ui.music_source_text),
              state->source_name, "PHONE MEDIA");
    copy_text(ui.music_title_text, sizeof(ui.music_title_text),
              state->track_title, "--");
    copy_text(ui.music_artist_text, sizeof(ui.music_artist_text),
              state->artist_name, "--");
    ui.music.source_name = ui.music_source_text;
    ui.music.track_title = ui.music_title_text;
    ui.music.artist_name = ui.music_artist_text;
    update_music_view();
}

extern "C" void moto_nav_ui_set_music_page_enabled(uint8_t enabled) {
    if(ui.screen == nullptr) return;
    // `enabled` is the media capability signal from PhoneNavBridge; the user's
    // layout switch gates it, so the effective visibility is the AND of both.
    ui.media_music_available = enabled != 0;
    ui.music_page_enabled = ui.layout.music_page_enabled != 0 &&
                            ui.media_music_available;
    if(!ui.music_page_enabled && ui.page == MOTO_UI_PAGE_MUSIC) {
        show_page(MOTO_UI_PAGE_NAVIGATION, true);
    }
    update_page_dots();
}

// ---------------------------------------------------------------------------
// Layout application entry points (plan §6.2). The layout is copied, clamped
// and applied in place; safe to call before moto_nav_ui_create, in which case
// the retained layout is applied when the objects exist.
// ---------------------------------------------------------------------------

namespace {

void apply_layout_internal(const moto_ui_layout_t *incoming) {
    navui_trace("apply: enter");
    if(incoming == nullptr) return;
    moto_ui_layout_t next = *incoming;

    // Clamp to the protocol ranges so a stale phone can never produce an
    // invalid viewport or an invisible component.
    if(next.component_count > MOTO_UI_LAYOUT_COMPONENT_COUNT) {
        next.component_count = MOTO_UI_LAYOUT_COMPONENT_COUNT;
    }
    next.map_height_px = static_cast<std::uint16_t>(
        std::clamp<int>(next.map_height_px, MOTO_UI_LAYOUT_MIN_MAP_HEIGHT,
                        MOTO_UI_LAYOUT_MAX_MAP_HEIGHT));
    if((next.map_height_px & 1U) != 0U) next.map_height_px -= 1;
    next.map_opacity = next.map_opacity > 100 ? 100 : next.map_opacity;
    next.zoom_percent = std::clamp<int>(next.zoom_percent, 50, 200);
    for(std::uint8_t i = 0; i < next.component_count; ++i) {
        moto_ui_component_layout_t &c = next.components[i];
        if(c.size > MOTO_UI_SIZE_L) c.size = MOTO_UI_SIZE_M;
        if(c.opacity > 100) c.opacity = 100;
        if(c.opacity < 10) c.opacity = 10;
        if(c.anchor > MOTO_UI_ARC_LEFT) c.anchor = MOTO_UI_ARC_TOP;
        c.span_deg = clamp_span(c.span_deg);
    }

    ui.layout = next;
    ui.layout_applied = true;

    // Map viewport + layers.
    // map_height_px is already expressed in 466-canvas pixels (§4.1), so it is
    // used verbatim; only the authored design-pixel helpers go through px().
    const int map_height = static_cast<int>(ui.layout.map_height_px);
    if(ui.nav_map != nullptr) {
        lv_obj_set_height(ui.nav_map, map_height);
        // The map viewport itself can be switched off entirely (protocol flag
        // bit2); the components below it then float over the plain page.
        if(ui.layout.map_visible) {
            lv_obj_clear_flag(ui.nav_map, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.nav_map, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(ui.nav_map_base != nullptr) {
        lv_obj_set_height(ui.nav_map_base, map_height);
        lv_obj_set_style_opa(ui.nav_map_base, opacity_opa(ui.layout.map_opacity), 0);
    }
    if(ui.nav_route_layer != nullptr) {
        lv_obj_set_height(ui.nav_route_layer, map_height);
        lv_obj_set_style_opa(ui.nav_route_layer, opacity_opa(ui.layout.route_opacity), 0);
    }
    // Route / street styling (§1.e).
    if(ui.nav_route != nullptr) {
        lv_obj_set_style_line_width(ui.nav_route, ui.layout.route_width_px, 0);
        lv_obj_set_style_line_color(
            ui.nav_route,
            rgb_color(ui.layout.route_r, ui.layout.route_g, ui.layout.route_b), 0);
    }
    if(ui.nav_route_shadow != nullptr) {
        lv_obj_set_style_line_width(ui.nav_route_shadow,
                                    ui.layout.route_width_px + 7, 0);
    }
    if(ui.nav_map_base != nullptr) {
        const bool roads = (ui.layout.layer_mask & 0x01) != 0;
        const bool buildings = (ui.layout.layer_mask & 0x02) != 0;
        for(std::uint8_t i = 0; i < MOTO_UI_ROAD_POLYLINE_CAPACITY; ++i) {
            if(ui.nav_roads[i] == nullptr) continue;
            lv_obj_set_style_line_width(ui.nav_roads[i], ui.layout.street_width_px, 0);
            lv_obj_set_style_line_color(
                ui.nav_roads[i],
                rgb_color(ui.layout.street_r, ui.layout.street_g, ui.layout.street_b), 0);
            lv_obj_set_style_line_opa(ui.nav_roads[i], opacity_opa(ui.layout.street_opacity), 0);
            if(!roads) {
                lv_obj_add_flag(ui.nav_roads[i], LV_OBJ_FLAG_HIDDEN);
            } else if(ui.nav_road_polyline_count > i) {
                lv_obj_clear_flag(ui.nav_roads[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        for(std::uint8_t i = 0; i < MOTO_UI_BUILDING_FOOTPRINT_CAPACITY; ++i) {
            if(ui.nav_buildings[i] == nullptr) continue;
            lv_obj_set_style_line_opa(ui.nav_buildings[i],
                                      opacity_opa(ui.layout.building_opacity), 0);
            if(!buildings) {
                lv_obj_add_flag(ui.nav_buildings[i], LV_OBJ_FLAG_HIDDEN);
            } else if(ui.nav_building_footprint_count > i) {
                lv_obj_clear_flag(ui.nav_buildings[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if(ui.nav_route_layer != nullptr && (ui.layout.layer_mask & 0x04) == 0) {
        lv_obj_add_flag(ui.nav_route_layer, LV_OBJ_FLAG_HIDDEN);
    } else if(ui.nav_route_layer != nullptr) {
        lv_obj_clear_flag(ui.nav_route_layer, LV_OBJ_FLAG_HIDDEN);
    }

    // Arc pairing: two arcs on the same side get inner/outer diameters.
    const moto_ui_component_layout_t *progress =
        find_component(ui.layout, MOTO_UI_COMPONENT_PROGRESS_ARC);
    const moto_ui_component_layout_t *speed_arc =
        find_component(ui.layout, MOTO_UI_COMPONENT_SPEED_ARC);
    const bool same_side = progress != nullptr && speed_arc != nullptr &&
                           progress->enabled && speed_arc->enabled &&
                           progress->anchor == speed_arc->anchor;

    // Per-component application.
    for(std::uint8_t i = 0; i < ui.layout.component_count; ++i) {
        const moto_ui_component_layout_t &c = ui.layout.components[i];
        switch(c.id) {
            case MOTO_UI_COMPONENT_SPEED:
                place_text_component(ui.nav_speed, c, ui.nav_speed_unit);
                break;
            case MOTO_UI_COMPONENT_SPEED_LIMIT: {
                if(ui.nav_limit == nullptr) break;
                if(!c.enabled) {
                    lv_obj_add_flag(ui.nav_limit, LV_OBJ_FLAG_HIDDEN);
                    break;
                }
                lv_obj_clear_flag(ui.nav_limit, LV_OBJ_FLAG_HIDDEN);
                int limit_width = 0, limit_height = 0;
                component_box(c.id, c.size, limit_width, limit_height);
                lv_obj_set_size(ui.nav_limit, limit_width, limit_height);
                lv_obj_set_pos(ui.nav_limit, c.x, c.y);
                lv_obj_set_style_opa(ui.nav_limit, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_opa(ui.nav_limit, opacity_opa(c.opacity), 0);
                lv_obj_set_style_border_opa(ui.nav_limit,
                                            opacity_opa(c.opacity), 0);
                if(ui.nav_limit_value != nullptr) {
                    lv_obj_set_style_text_opa(
                        ui.nav_limit_value,
                        text_opacity_opa(c.text_opacity), 0);
                    lv_obj_center(ui.nav_limit_value);
                }
                break;
            }
            case MOTO_UI_COMPONENT_HEADING:
                place_text_component(ui.nav_heading, c,
                                     ui.nav_heading_cardinal);
                break;
            case MOTO_UI_COMPONENT_MANEUVER: {
                apply_component(ui.nav_maneuver, c);
                // §4.3: the maneuver glyph is authored for the M bounding box
                // and scales with the tier, so S/L keep the same artwork.
                int box_width = 0, box_height = 0;
                component_box(c.id, c.size, box_width, box_height);
                const int medium_width = 180;
                ui.maneuver_scale = medium_width > 0 && box_width > 0
                                        ? static_cast<float>(box_width) /
                                              static_cast<float>(medium_width)
                                        : 1.0F;
                if(ui.nav_maneuver != nullptr) {
                    lv_obj_invalidate(ui.nav_maneuver);
                }
                // The distance/unit are text children of the maneuver box.
                if(ui.nav_distance != nullptr) {
                    lv_obj_set_style_text_opa(
                        ui.nav_distance, text_opacity_opa(c.text_opacity), 0);
                }
                if(ui.nav_unit != nullptr) {
                    lv_obj_set_style_text_opa(
                        ui.nav_unit, text_opacity_opa(c.text_opacity), 0);
                }
                break;
            }
            case MOTO_UI_COMPONENT_REMAINING_DISTANCE:
                place_text_component(ui.nav_remaining_distance, c,
                                     ui.nav_remaining_distance_caption);
                break;
            case MOTO_UI_COMPONENT_REMAINING_TIME:
                place_text_component(ui.nav_remaining_time, c,
                                     ui.nav_remaining_time_caption);
                break;
            case MOTO_UI_COMPONENT_PROGRESS_ARC:
                if(ui.nav_progress != nullptr) {
                    if(!c.enabled) {
                        lv_obj_add_flag(ui.nav_progress, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_clear_flag(ui.nav_progress, LV_OBJ_FLAG_HIDDEN);
                        apply_arc(ui.nav_progress, c, 440, c.opacity);
                    }
                }
                break;
            case MOTO_UI_COMPONENT_SPEED_ARC:
                if(ui.nav_speed_arc_object != nullptr) {
                    if(!c.enabled) {
                        lv_obj_add_flag(ui.nav_speed_arc_object, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        // Position the acceleration readout once, from
                        // the arc geometry: per-frame alignment would let
                        // it drift between otherwise identical renders.
                        if(ui.nav_accel != nullptr) {
                            const int offset =
                                (same_side ? 408 : 440) / 6;
                            int dx = 0;
                            int dy = 0;
                            switch(c.anchor) {
                                case MOTO_UI_ARC_TOP:
                                    dy = -offset;
                                    break;
                                case MOTO_UI_ARC_BOTTOM:
                                    dy = offset;
                                    break;
                                case MOTO_UI_ARC_LEFT:
                                    dx = -offset;
                                    break;
                                case MOTO_UI_ARC_RIGHT:
                                    dx = offset;
                                    break;
                                default:
                                    break;
                            }
                            lv_obj_align_to(ui.nav_accel,
                                            ui.nav_speed_arc_object,
                                            LV_ALIGN_CENTER, dx, dy);
                        }
                        lv_obj_clear_flag(ui.nav_speed_arc_object, LV_OBJ_FLAG_HIDDEN);
                        apply_arc(ui.nav_speed_arc_object, c,
                                  same_side ? 408 : 440, c.opacity);
                    }
                }
                break;
            default:
                break;
        }
    }

    navui_trace("apply: components done");
    // Music page visibility = user switch AND media capability.
    ui.music_page_enabled =
        ui.layout.music_page_enabled != 0 && ui.media_music_available;
    update_page_dots();
    align_accel_readout();
    // The marker stays at the centre of the map viewport (§4.8) at every map
    // height; height changes zoom the window around the rider.
    if(ui.nav_marker != nullptr) {
        const int marker_y = map_height / 2;
        lv_obj_set_pos(ui.nav_marker, MOTO_UI_CANVAS_WIDTH / 2 - px(27),
                       marker_y - px(27));
    }
}

}  // namespace

extern "C" void moto_nav_ui_apply_layout(const moto_ui_layout_t *layout) {
    apply_layout_internal(layout);
}

extern "C" const moto_ui_layout_t *moto_nav_ui_current_layout(void) {
    return &ui.layout;
}

extern "C" void moto_nav_ui_set_music_command_callback(
    moto_music_command_callback_t callback, void *context) {
    ui.music_callback = callback;
    ui.music_callback_context = context;
}

extern "C" void moto_nav_ui_set_demo_active(uint8_t enabled) {
    ui.demo_active = enabled != 0;
    refresh_lifecycle();
}

extern "C" void moto_nav_ui_set_demo_change_callback(
    moto_demo_change_callback_t callback, void *context) {
    ui.demo_callback = callback;
    ui.demo_callback_context = context;
}
