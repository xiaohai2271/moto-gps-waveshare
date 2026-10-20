#include "sim_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace moto::sim {
namespace {

bool g_verbose = false;
bool g_verbose_initialised = false;

bool env_verbose() {
  const char* value = std::getenv("MOTO_SIM_VERBOSE");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

}  // namespace

void set_verbose(bool enabled) { g_verbose = enabled; }

bool verbose() {
  if(!g_verbose_initialised) {
    g_verbose_initialised = true;
    g_verbose = g_verbose || env_verbose();
  }
  return g_verbose;
}

void log_line(const char* level, const char* tag, const char* format, ...) {
  if(!verbose()) {
    return;
  }
  std::fprintf(stderr, "[%s] %s: ", level == nullptr ? "?" : level,
               tag == nullptr ? "sim" : tag);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
}

}  // namespace moto::sim
