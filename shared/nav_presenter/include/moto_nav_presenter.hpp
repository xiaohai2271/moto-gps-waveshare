#pragma once

#include "moto_nav_ui.h"
#include "nav_core/nav_core.hpp"

#include <string>

namespace moto::nav {

// Deterministic bridge between the shared navigation model and the shared
// LVGL view. The char pointers in ui_state() remain valid until the next
// update() call or destruction of this presenter.
class NavPresenter {
 public:
  NavPresenter();
  NavPresenter(const NavPresenter&) = delete;
  NavPresenter& operator=(const NavPresenter&) = delete;
  NavPresenter(NavPresenter&&) = delete;
  NavPresenter& operator=(NavPresenter&&) = delete;

  void update(const NavSnapshot& snapshot);
  [[nodiscard]] const moto_ui_state_t& ui_state() const;

  // Navigation page view configuration (docs §4.7/§4.8/§6.3).
  //   north_up:      true = map north-up + rotated vehicle marker,
  //                  false = map rotates with the phone heading.
  //   map_height_px: 466-canvas map viewport height, 160..320.
  //   zoom_percent:  50..200, 100 = the existing 0.44 px/m street scale.
  // The projection is recomputed by the next update(), so switching the mode
  // never rebuilds LVGL objects or interrupts the 40 Hz motion path.
  void set_view_config(bool north_up, int map_height_px, int zoom_percent);
  [[nodiscard]] bool north_up() const { return north_up_; }
  [[nodiscard]] int map_height_px() const { return map_height_px_; }

  // Applies the last mapped state to the platform-independent LVGL UI API.
  // LVGL initialization remains the responsibility of the platform adapter.
  void apply_to_lvgl() const;

 private:
  std::string road_name_;
  std::string next_road_name_;
  moto_ui_state_t ui_state_{};
  bool north_up_ = true;
  int map_height_px_ = MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT;
  int zoom_percent_ = 100;
};

}  // namespace moto::nav
