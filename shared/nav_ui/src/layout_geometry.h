#pragma once

// Navigation-page layout geometry helpers (docs/NAV_LAYOUT_EDITOR_PLAN.md §4).
//
// The 466 x 466 canvas coordinates, component bounding boxes, arc anchors and
// the clamp rules are duplicated from the Android side (LayoutDefaults /
// ComponentSizes) on purpose: the phone preview and the device must agree
// without any runtime exchange beyond the wire layout itself. Section 4 is the
// single source of truth for both copies.
//
// Everything here is constexpr/free of LVGL so the host tests can exercise the
// same geometry the firmware renders.

#include <cstdint>

#include "moto_nav_ui.h"

namespace moto::nav::ui {

// Canvas fixed points (§4.1).
constexpr int kCanvasWidth = MOTO_UI_CANVAS_WIDTH;
constexpr int kCanvasHeight = MOTO_UI_CANVAS_HEIGHT;
constexpr int kCanvasCenter = kCanvasWidth / 2;
constexpr int kSafeRadius = 220;

// Arc geometry (§4.3/§4.5). Both arcs are drawn on the canvas centre; when they
// dock to the same side the inner one shrinks so the two never overlap.
constexpr int kArcDiameter = 440;
constexpr int kArcDiameterInner = 408;

// Per-component bounding boxes in canvas pixels (§4.3).
struct ComponentBox {
    int width;
    int height;
};

constexpr ComponentBox component_box(int component_id, int size) {
    const int tier = size < MOTO_UI_SIZE_S
                         ? static_cast<int>(MOTO_UI_SIZE_S)
                     : size > MOTO_UI_SIZE_L
                         ? static_cast<int>(MOTO_UI_SIZE_L)
                         : size;
    switch(component_id) {
        case MOTO_UI_COMPONENT_SPEED:
            return tier == static_cast<int>(MOTO_UI_SIZE_S) ? ComponentBox{72, 56}
                 : tier == static_cast<int>(MOTO_UI_SIZE_L) ? ComponentBox{124, 92}
                                                             : ComponentBox{96, 72};
        case MOTO_UI_COMPONENT_SPEED_LIMIT:
            return tier == static_cast<int>(MOTO_UI_SIZE_S) ? ComponentBox{44, 44}
                 : tier == static_cast<int>(MOTO_UI_SIZE_L) ? ComponentBox{78, 78}
                                                             : ComponentBox{61, 61};
        case MOTO_UI_COMPONENT_HEADING:
            return tier == static_cast<int>(MOTO_UI_SIZE_S) ? ComponentBox{68, 60}
                 : tier == static_cast<int>(MOTO_UI_SIZE_L) ? ComponentBox{112, 96}
                                                             : ComponentBox{88, 76};
        case MOTO_UI_COMPONENT_MANEUVER:
            return tier == static_cast<int>(MOTO_UI_SIZE_S) ? ComponentBox{150, 108}
                 : tier == static_cast<int>(MOTO_UI_SIZE_L) ? ComponentBox{210, 148}
                                                             : ComponentBox{180, 128};
        case MOTO_UI_COMPONENT_REMAINING_DISTANCE:
            return tier == static_cast<int>(MOTO_UI_SIZE_S) ? ComponentBox{92, 44}
                 : tier == static_cast<int>(MOTO_UI_SIZE_L) ? ComponentBox{146, 68}
                                                             : ComponentBox{116, 56};
        case MOTO_UI_COMPONENT_REMAINING_TIME:
            return tier == static_cast<int>(MOTO_UI_SIZE_S) ? ComponentBox{78, 44}
                 : tier == static_cast<int>(MOTO_UI_SIZE_L) ? ComponentBox{128, 68}
                                                             : ComponentBox{100, 56};
        case MOTO_UI_COMPONENT_PROGRESS_ARC:
        case MOTO_UI_COMPONENT_SPEED_ARC:
        default:
            // Arcs are sized by diameter/width, never by a bounding box.
            return ComponentBox{0, 0};
    }
}

// Arc stroke width per size tier (§4.3): S/M/L = 4/6/8.
constexpr int arc_stroke_width(int size) {
    const int tier = size < MOTO_UI_SIZE_S
                         ? static_cast<int>(MOTO_UI_SIZE_S)
                     : size > MOTO_UI_SIZE_L
                         ? static_cast<int>(MOTO_UI_SIZE_L)
                         : size;
    return tier == static_cast<int>(MOTO_UI_SIZE_S)
               ? 4
               : tier == static_cast<int>(MOTO_UI_SIZE_L) ? 8 : 6;
}

// LVGL angle convention: 0 deg points right and angles grow clockwise, so the
// four docked anchors sit at 270/0/90/180 (§4.5).
constexpr int anchor_center_angle(int anchor) {
    switch(anchor) {
        case MOTO_UI_ARC_RIGHT: return 0;
        case MOTO_UI_ARC_BOTTOM: return 90;
        case MOTO_UI_ARC_LEFT: return 180;
        case MOTO_UI_ARC_TOP:
        default: return 270;
    }
}

// Only the three documented spans exist; anything else falls back to 120 deg so
// an out-of-range phone value still renders a sane arc (§4.5).
constexpr int normalize_span_deg(int span_deg) {
    return span_deg == 90 ? 90 : span_deg == 180 ? 180 : 120;
}

constexpr bool is_arc_component(int component_id) {
    return component_id == MOTO_UI_COMPONENT_PROGRESS_ARC ||
           component_id == MOTO_UI_COMPONENT_SPEED_ARC;
}

}  // namespace moto::nav::ui
