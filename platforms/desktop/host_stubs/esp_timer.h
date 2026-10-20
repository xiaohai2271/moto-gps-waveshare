#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Monotonic microseconds. The host implementation is backed by
// std::chrono::steady_clock so firmware time arithmetic keeps working.
int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
