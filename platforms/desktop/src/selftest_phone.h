#pragma once

#include <string>

namespace moto::sim {

class DeviceSim;

// Runs a scripted phone against the virtual device. The caller advances the
// device clock; the script drives the real wire protocol and verifies the
// resulting firmware/UI state. Returns true when every step passed.
bool run_selftest(DeviceSim& sim, bool dump_frames, std::string* error);

}  // namespace moto::sim
