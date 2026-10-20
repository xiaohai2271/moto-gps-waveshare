#pragma once

#include <cstdint>

namespace moto::sim::wifi {

// Drives the scripted WiFi provisioning fake. Call once per virtual frame so
// status callbacks fire from the simulator's main loop, never from inside a
// BLE message handler (mirroring the firmware's dedicated WiFi task).
void pump(std::uint64_t now_ms);

// Test hook: forget persisted credentials between self-test runs.
void reset();

}  // namespace moto::sim::wifi
