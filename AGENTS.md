# AGENTS.md — hardware-free test contract

本文件面向在本仓库工作的 AI/自动化 agent。目标：**在没有 ESP32、没有真机圆屏的情况下，
把除射频与面板之外的改动验证到位**，并且不把模拟结果冒充硬件验收。

This file is the contract for AI agents working in this repository. Use the host
virtual device and the simulator before asking for an ESP32, a phone, or a BLE
peripheral. Simulator success is **not** hardware acceptance.

---

## 1. Non-negotiable rules

1. **Never claim hardware verification.** The simulator covers the application,
   protocol, bridge, and LVGL UI. It does not cover BLE RF, pairing, background
   liveness, the CO5300 panel, PSRAM pressure, the IMU, or power.
2. **Baseline first.** Run the existing suite before editing; rerun the matching
   checks after. Do not lower a test to make a change pass.
3. **Keep the layers.** Protocol and business logic live in `shared/`. Host stubs
   may implement only platform APIs (`board_port`, `esp_*`, WiFi radio). Never
   copy protocol or navigation logic into Kotlin, JavaScript, or a second C++ path.
4. **Protocol changes are byte-level changes.** Update
   `shared/protocol/fixtures/ble-navigation-v1.golden.txt` and both endpoints
   (`ble_protocol_tests`, Android `GoldenBytesJvmTest`) in the same change.
5. **CI-safe ports.** Self-tests and scripts must pass `--port 0` (ephemeral).
   Never bind a fixed 9100 in a test.
6. **Do not commit generated output.** `build/`, `*.bmp`, APKs, `.cxx/`, Gradle
   caches and `platforms/android/local.properties` stay untracked.
7. **No ESP-IDF/NimBLE/NVS dependencies in host code.** The virtual device links
   the real firmware sources, not a second implementation.
8. **If a check cannot run, say so.** "Not run" is a valid result; a fabricated
   pass is not.

---

## 2. Quick start

Linux / macOS:

```sh
cmake -S . -B build/native -DMOTO_BUILD_TESTS=ON -DMOTO_BUILD_SIM=ON
cmake --build build/native --parallel 4
ctest --test-dir build/native --output-on-failure
./build/native/bin/moto_device_sim --list-cases
./build/native/bin/moto_device_sim --case baseline --port 0
```

Windows (VS Developer PowerShell; in restricted environments Ninja's
`try_compile` can hang, so NMake is the reliable default):

```powershell
cmake -S . -B build/native -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release `
      -DMOTO_BUILD_TESTS=ON -DMOTO_BUILD_SIM=ON
cmake --build build/native
<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe `
    --test-dir build/native --output-on-failure
build\native\bin\moto_device_sim.exe --list-cases
build\native\bin\moto_device_sim.exe --case baseline --port 0
```

If the `third_party/lvgl` directory is empty, initialise the submodule first
(requires network). Do not add a second LVGL copy.

Android (after the host simulator is running on 9100):

```powershell
cd platforms\android
.\gradlew.bat :app:assembleDebug :app:testDebugUnitTest
adb reverse tcp:9100 tcp:9100
adb shell run-as dev.glimpse.motogps sh -c "echo 127.0.0.1:9100 > files/ble_sim"
# scan "MOTO GPS (virtual)" and connect; delete files/ble_sim to restore BLE
```

Web UI simulator (Emscripten):

```sh
./scripts/build_web.sh
python -m http.server 4173 --directory .
# http://127.0.0.1:4173/platforms/web/shell/index.html?demo=1
```

---

## 3. Required checks by touched area

| Touched | Must run |
| --- | --- |
| `shared/nav_core`, `shared/nav_app`, `shared/nav_presenter` | `ctest` (includes all simulator cases) |
| `shared/nav_ui`, fonts, layout, projection | `ctest` + `moto_device_sim --case baseline --frames build/native/frames` and `--case layout-rules`; inspect `01-offline-baseline.bmp` / `02-baseline.bmp` |
| `shared/ble_protocol` | `ble_protocol_tests` + Android `GoldenBytesJvmTest` + `ctest` |
| `platforms/esp32/main/phone_nav_bridge.*` | `phone_nav_bridge_tests` + `ctest` |
| `platforms/esp32/main/ble_nav_transport_nimble.*` | No host coverage. Build ESP-IDF and mark hardware-unverified. If transport semantics changed, mirror them in `VirtualBleTransport` and add/extend a case. |
| `platforms/esp32/main/wifi_provisioning.*`, `wifi_health_server.*` | No host coverage for the radio; extend the scripted fake + `wifi-errors` case, and mark the RF path unverified. |
| Android UI / navigation / layout | `:app:testDebugUnitTest` + `:app:assembleDebug` |
| Android `TcpBleTransport` / `Esp32BleCentral` | above + host simulator E2E via the `files/ble_sim` marker |
| `backend/` | `npm --prefix backend test` |
| docs only | no build; check links and keep zh/en in sync |

A change that touches more than one row runs the union. When in doubt, run the
whole suite; it is only a few seconds.

---

## 4. Extending the simulator

- **New/changed application message**: add a case in
  `platforms/desktop/src/selftest_phone.cpp` using `NavPhoneClient`, and assert
  the visible result through `sim.presenter().ui_state()` or
  `sim.applied_layout()`.
- **New case registration**: append to `kCases` in `selftest_phone.cpp`, add the
  id to `MOTO_SIM_CASES` in `platforms/desktop/CMakeLists.txt`, and update
  `platforms/desktop/README.md` + this file.
- **Handshake / ACK / heartbeat / watchdog semantics**: only then touch
  `VirtualBleTransport`, and keep it aligned with
  `platforms/esp32/main/ble_nav_transport_nimble.cpp`.
- **New WiFi behaviour**: extend the SSID rules in
  `src/wifi_provisioning_host.cpp` (`fail`, `timeout`, otherwise success) and
  extend the `wifi-errors` case.
- **New link failure mode**: add a case that disconnects, delays, or drops
  frames, then assert recovery or the degraded state.
- **New UI state**: prefer asserting `moto_ui_state_t` fields over pixel hashes;
  only add a golden frame when a visual regression has no state proxy.

Do not make the simulator depend on Android, the network, a real AMap key, or a
wall-clock-dependent animation. Every case must run in its own process and stay
deterministic enough for CI.

---

## 5. Evidence to report

Every task must end with:

```text
Changed: <files / areas>
Commands:
  - ctest --test-dir build/native --output-on-failure -> 25/25 passed
  - moto_device_sim --case <id> --port 0             -> CASE PASS <id>
Artifacts: build/native/frames/02-<id>.bmp (if UI changed)
Not verified on hardware: BLE RF/pairing, CO5300 panel, PSRAM/40 Hz, IMU, power
```

Rules of thumb:

- paste the `100% tests passed` summary and the `CASE PASS <id>` line, not a
  paraphrase;
- if a test was skipped, state the exact reason (`no emulator`, `submodule not
  initialised`, `no network`);
- a screenshot is evidence of rendering, not of panel colour or refresh rate.

---

## 6. Failure triage

| Symptom | Likely cause / action |
| --- | --- |
| `bind(127.0.0.1:9100) failed` | port in use; use `--port 0` or stop the other instance |
| `CASE FAIL <id>: ...` | run that case alone with `--case <id> --verbose --port 0`; check session id, heartbeat cadence, frame size |
| Case times out with no frames | ensure `moto_nav_ui_create()` runs after the headless display; check LVGL heap |
| Android never scans | debug build? marker file present? `adb reverse --list`? logcat tag `MotoGpsSim` |
| Emulator cannot use `adb reverse` | put `10.0.2.2:9100` in `files/ble_sim` instead |
| `git status` fails on `third_party/lvgl` | submodule not initialised in this worktree; init it or work in the main checkout |
| Ninja configure hangs on Windows (seen in some restricted environments) | use `-G "NMake Makefiles"` |
| real LVGL symbol missing | do not add `tests/native/esp32_host_stubs` to the simulator include path; its `lvgl.h` is a stub |

---

## 7. Pointers

- `docs/TEST_STRATEGY.md` — dimension model, case matrix, closure criteria.
- `platforms/desktop/README.md` — simulator design, link framing, limits.
- `docs/TESTING.md` — full local test matrix.
- `docs/ANDROID_AI_GUIDE.md` — Android contribution rules and hardware claims.
- `shared/protocol/ble-navigation-v1.md` — wire protocol.
- `platforms/esp32/main/ble_nav_transport_nimble.cpp` — the transport contract
  `VirtualBleTransport` mirrors.

---

## 8. Delegation prompt (copy-paste)

```text
Work in this repository and follow AGENTS.md.

Goal: <one sentence>.

Before editing, run the baseline for the areas you will touch
(`ctest --test-dir build/native --output-on-failure`, and the Android/Gradle
checks from AGENTS.md §3). After editing, run the same checks plus the matching
host virtual-device cases:
  build/native/bin/moto_device_sim --list-cases
  build/native/bin/moto_device_sim --case <id> --port 0
If the change touches UI or protocol, also run with
  --frames build/native/frames
and inspect the generated frames.

Constraints:
- Do not claim hardware, BLE RF, panel, IMU, or power verification.
- Do not duplicate protocol/business logic into Kotlin/JS or a second C++ path.
- Protocol changes must update the golden fixture and both endpoints.
- Do not commit build output, frames, APKs, or local.properties.
- If a check cannot run, say exactly why; never fabricate a pass.

Report: changed files, exact commands + exit codes, the ctest summary and the
CASE PASS lines, artifact paths, and the list of items still requiring hardware.
```
