#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "device_sim.h"
#include "esp_timer.h"
#include "selftest_phone.h"
#include "sim_log.h"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) { g_stop.store(true); }

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
}

void print_usage() {
  std::printf(
      "moto_device_sim - host virtual device for the MOTO GPS round display\n"
      "\n"
      "Usage:\n"
      "  moto_device_sim [--port N] [--selftest | --case ID] [--frames DIR]\n"
      "                  [--seconds N] [--verbose]\n"
      "  moto_device_sim --list-cases\n"
      "\n"
      "Options:\n"
      "  --port N       TCP port for the phone link (0 = ephemeral, default 9100)\n"
      "  --selftest     Run the baseline case (alias for --case baseline)\n"
      "  --case ID      Run one deterministic end-to-end case and exit\n"
      "  --list-cases   List case ids and descriptions\n"
      "  --frames DIR   Write 466x466 BMP snapshots to DIR\n"
      "  --seconds N    Stop after N seconds (server mode)\n"
      "  --verbose      Log firmware diagnostics (or set MOTO_SIM_VERBOSE=1)\n"
      "  --help         Show this message\n"
      "\n"
      "The phone link carries one length-prefixed encoded BLE frame per\n"
      "message. See platforms/desktop/README.md.\n");
}

}  // namespace

int main(int argc, char** argv) {
  moto::sim::DeviceSimOptions options;
  std::string case_id;
  bool selftest = false;
  bool list_cases = false;
  bool verbose = false;
  std::string frames_dir;
  int seconds = 0;

  for(int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if(argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    }
    if(argument == "--selftest") {
      selftest = true;
      continue;
    }
    if(argument == "--list-cases") {
      list_cases = true;
      continue;
    }
    if(argument == "--verbose") {
      verbose = true;
      continue;
    }
    const auto value = [&](const char* name) -> const char* {
      if(index + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        std::exit(2);
      }
      return argv[++index];
    };
    if(argument == "--port") {
      options.port = static_cast<std::uint16_t>(std::atoi(value("--port")));
      continue;
    }
    if(argument == "--case") {
      case_id = value("--case");
      continue;
    }
    if(argument == "--frames") {
      frames_dir = value("--frames");
      options.frames_dir = frames_dir;
      continue;
    }
    if(argument == "--seconds") {
      seconds = std::atoi(value("--seconds"));
      continue;
    }
    std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
    print_usage();
    return 2;
  }

  moto::sim::set_verbose(verbose);

  if(list_cases) {
    for(const auto& entry : moto::sim::cases()) {
      std::printf("%-18s %s\n", entry.id.c_str(),
                  entry.description.c_str());
    }
    return 0;
  }

  if(selftest && case_id.empty()) {
    case_id = "baseline";
  }

  std::signal(SIGINT, handle_signal);
#ifdef SIGTERM
  std::signal(SIGTERM, handle_signal);
#endif

  moto::sim::DeviceSim sim;
  std::string error;
  if(!sim.init(options, &error)) {
    std::fprintf(stderr, "device init failed: %s\n", error.c_str());
    return 1;
  }

  if(!case_id.empty()) {
    std::string failure;
    const bool ok =
        moto::sim::run_case(sim, case_id, !frames_dir.empty(), &failure);
    if(!ok) {
      std::fprintf(stderr, "CASE FAIL %s: %s\n", case_id.c_str(),
                   failure.c_str());
      return 1;
    }
    std::printf("CASE PASS %s (port %u)\n", case_id.c_str(),
                static_cast<unsigned>(sim.port()));
    if(!frames_dir.empty()) {
      std::printf("Frames: %s\n", frames_dir.c_str());
    }
    return 0;
  }

  std::printf("moto_device_sim listening on 127.0.0.1:%u\n",
              static_cast<unsigned>(sim.port()));
  std::printf("Connect the Android debug build with: adb reverse tcp:%u tcp:%u\n",
              static_cast<unsigned>(sim.port()),
              static_cast<unsigned>(sim.port()));
  std::fflush(stdout);

  const std::uint64_t started_ms = now_ms();
  while(!g_stop.load()) {
    sim.tick(now_ms());
    if(seconds > 0 && now_ms() - started_ms >=
                          static_cast<std::uint64_t>(seconds) * 1'000U) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return 0;
}
