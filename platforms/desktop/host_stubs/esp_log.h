#pragma once

// Logging shim for the host virtual device. Firmware sources call ESP_LOGI()
// and friends; on the host they are routed through a single, optional sink so
// the simulator stays quiet during CI unless MOTO_SIM_VERBOSE is set.

#include "sim_log.h"

#define ESP_LOGD(tag, ...) moto::sim::log_line("D", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) moto::sim::log_line("I", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) moto::sim::log_line("W", tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) moto::sim::log_line("E", tag, __VA_ARGS__)
