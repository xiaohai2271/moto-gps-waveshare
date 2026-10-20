#pragma once

namespace moto::sim {

// Optional diagnostics for the virtual device. Off by default so CI output
// stays limited to the self-test verdict; enable with MOTO_SIM_VERBOSE=1 or
// --verbose.
void set_verbose(bool enabled);
[[nodiscard]] bool verbose();

void log_line(const char* level, const char* tag, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

}  // namespace moto::sim
