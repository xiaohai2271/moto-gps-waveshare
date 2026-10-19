#pragma once

#include "moto_nav_ui.h"

namespace moto::test {

void reset_nav_ui_probe();
int nav_ui_apply_count();
const moto_ui_state_t& last_applied_nav_ui_state();

// Layout engine probes: what the renderer would have been told to apply.
int nav_ui_layout_apply_count();
const moto_ui_layout_t& last_applied_nav_ui_layout();

}  // namespace moto::test
