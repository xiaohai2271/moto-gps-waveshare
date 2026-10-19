#pragma once

// GPS speed-difference acceleration filter.
//
// This header is intentionally free of LVGL and BLE dependencies so the exact
// same code runs on the ESP32-S3 and in the host render tests. The device does
// not extend NavigationSnapshot with acceleration: PhoneNavBridge owns the
// navigation model and its snapshots stay unchanged.
//
// Design (docs/NAV_LAYOUT_EDITOR_PLAN.md §6.4):
//   * a 1.5 s sliding window of GPS speed samples computes the raw slope;
//   * an EMA smooths the slope so 5 Hz course/speed noise cannot flash the arc;
//   * the result saturates at +/- 20 m/s^2 and decays to zero at standstill.

#include <cstdint>
#include <cstdio>

namespace moto::nav::ui {

struct AccelSample {
    std::uint32_t tick_ms = 0;
    float speed_mps = 0.0F;
};

struct AccelFilter {
    static constexpr int kSampleCapacity = 16;
    static constexpr std::uint32_t kWindowMs = 1'500;
    static constexpr float kMaxAccelMps2 = 20.0F;
    static constexpr float kStandstillMps = 0.6F;
    static constexpr float kEmaAlpha = 0.35F;

    AccelSample samples[kSampleCapacity]{};
    int sample_count = 0;
    int next_sample = 0;
    std::uint32_t last_tick_ms = 0;
    float speed_mps = 0.0F;
    float accel_mps2 = 0.0F;
    bool initialized = false;
};

constexpr float accel_speed_kph_to_mps(float speed_kph) {
    return speed_kph / 3.6F;
}

inline void accel_filter_reset(AccelFilter &filter) {
    filter = AccelFilter{};
}

// Push one GPS speed sample. Consecutive calls with an unchanged tick are
// ignored, which keeps repeated snapshot renders from biasing the window.
inline void accel_filter_push(AccelFilter &filter, float speed_kph,
                              std::uint32_t tick_ms) {
    if (speed_kph < 0.0F) speed_kph = 0.0F;
    const float current_mps = accel_speed_kph_to_mps(speed_kph);
    if (!filter.initialized) {
        filter.initialized = true;
        filter.last_tick_ms = tick_ms;
        filter.speed_mps = current_mps;
        filter.samples[0] = {tick_ms, current_mps};
        filter.sample_count = 1;
        filter.next_sample = 1;
        filter.accel_mps2 = 0.0F;
        return;
    }
    if (tick_ms == filter.last_tick_ms) return;
    if (static_cast<std::int32_t>(tick_ms - filter.last_tick_ms) < 0) {
        // A wrapped or reordered tick must not produce a negative interval.
        filter.last_tick_ms = tick_ms;
        return;
    }
    filter.samples[filter.next_sample] = {tick_ms, current_mps};
    filter.next_sample = (filter.next_sample + 1) % AccelFilter::kSampleCapacity;
    if (filter.sample_count < AccelFilter::kSampleCapacity) {
        ++filter.sample_count;
    }
    filter.last_tick_ms = tick_ms;
    filter.speed_mps = current_mps;

    // Standstill: GPS course/speed noise would otherwise report random
    // accelerations while the bike is parked.
    if (current_mps <= AccelFilter::kStandstillMps) {
        filter.accel_mps2 = 0.0F;
        return;
    }

    // Find the oldest retained sample that still lies inside the window.
    // The ring buffer is chronological from (next_sample - sample_count).
    int oldest = (filter.next_sample - filter.sample_count +
                  AccelFilter::kSampleCapacity) %
                 AccelFilter::kSampleCapacity;
    const int newest =
        (filter.next_sample - 1 + AccelFilter::kSampleCapacity) %
        AccelFilter::kSampleCapacity;
    for (int index = 0; index < filter.sample_count; ++index) {
        const int position =
            (oldest + index) % AccelFilter::kSampleCapacity;
        if (tick_ms - filter.samples[position].tick_ms <=
            AccelFilter::kWindowMs) {
            oldest = position;
            break;
        }
    }
    const float dt_s = static_cast<float>(
        tick_ms - filter.samples[oldest].tick_ms) / 1'000.0F;
    const float speed_delta =
        filter.samples[newest].speed_mps - filter.samples[oldest].speed_mps;
    float raw = dt_s > 0.05F ? speed_delta / dt_s : 0.0F;
    if (raw > AccelFilter::kMaxAccelMps2) raw = AccelFilter::kMaxAccelMps2;
    if (raw < -AccelFilter::kMaxAccelMps2) raw = -AccelFilter::kMaxAccelMps2;
    filter.accel_mps2 +=
        (raw - filter.accel_mps2) * AccelFilter::kEmaAlpha;
    if (filter.accel_mps2 > -0.05F && filter.accel_mps2 < 0.05F) {
        filter.accel_mps2 = 0.0F;
    }
}

// Called when the GPS fix is lost: the next usable sample starts a fresh
// window instead of differentiating across the gap.
inline void accel_filter_invalidate(AccelFilter &filter) {
    filter.initialized = false;
    filter.sample_count = 0;
    filter.next_sample = 0;
    filter.accel_mps2 = 0.0F;
}

inline float accel_filter_accel(const AccelFilter &filter) {
    return filter.accel_mps2;
}

}  // namespace moto::nav::ui
