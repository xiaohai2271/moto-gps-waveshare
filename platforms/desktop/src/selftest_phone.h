#pragma once

#include <string>
#include <vector>

namespace moto::sim {

class DeviceSim;

struct SimCaseInfo {
  std::string id;
  std::string description;
};

// Deterministic end-to-end cases driven by the scripted phone.
[[nodiscard]] std::vector<SimCaseInfo> cases();

// Runs one case against a freshly initialised virtual device. The caller owns
// the device and advances its clock; this function starts the phone thread,
// ticks the device until the script finishes, verifies the UI state, and stops
// the phone before returning.
bool run_case(DeviceSim& sim, const std::string& id, bool dump_frames,
              std::string* error);

// Backwards-compatible alias for the baseline case.
bool run_selftest(DeviceSim& sim, bool dump_frames, std::string* error);

}  // namespace moto::sim
