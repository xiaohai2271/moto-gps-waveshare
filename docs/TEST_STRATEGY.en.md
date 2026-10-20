> **Language:** English · [中文](TEST_STRATEGY.md)

# Test strategy: simulation-first functional closure

This document defines what "done" means for this project, the dimensions every
change must be tested against, and the case matrix that runs on the host.

The Chinese edition ([`TEST_STRATEGY.md`](TEST_STRATEGY.md)) is authoritative if
the two differ.

## 1. Definition of done

A feature or fix is done only when:

1. it is mapped onto the six dimensions below;
2. every new state, error branch and boundary has an executable case;
3. the local `ctest` suite passes (currently 25 tests, including 15 simulator
   end-to-end cases);
4. protocol fields change together with the golden fixture and both endpoints;
5. the report contains commands, exit codes, `CASE PASS`/`ctest` output,
   artifact paths, and the **not verified** list;
6. hardware-only acceptance items are listed explicitly.

"Compiles" is not done; "self-test ran without assertions" is not done either.

## 2. Six dimensions

| Dimension | Focus | Main tool |
| --- | --- | --- |
| Functional states | Idle / Acquiring / Planning / Navigating / Rerouting / Arrived / Offline; pages | Simulator state cases + `NavPresenter` assertions |
| Protocol and timing | handshake, session, heartbeat, watchdog, ACK/retry, duplicate, out-of-order, stale generation, fragmentation | Byte-level cases + `ble_protocol_tests` + Android golden test |
| Data boundaries | E6 coordinates, maximum points/roads/buildings/components, string lengths, enums, empty values, saturation | `fragmentation` + decoder tests |
| Failure and recovery | link loss, reconnect, new session, geometry generation mismatch, layout rollback/invalid, WiFi auth/timeout/scan/forget | Fault-injection cases that assert recovery |
| Platforms and interaction | ESP32 bridge, Android Compose/BLE/location/gateway, WASM web, desktop simulator; touch/swipe | Simulator, Android unit/instrumented tests, WASM simulator |
| Non-functional | frame rate, memory/PSRAM, power, temperature, RF, panel colour and TE | Hardware only; the simulator provides frame/state regressions, not physical conclusions |

## 3. Case matrix

Each case is an independent process:

```sh
build/native/bin/moto_device_sim --list-cases
build/native/bin/moto_device_sim --case <id> --port 0
```

| ID | Dimension | Scenario | Key assertions |
| --- | --- | --- | --- |
| `baseline` | Function/protocol/data | handshake → snapshot → 3 geometry chunks → map → media → layout → WiFi success/failure | mode, maneuver, distance, road name, route points, layout, online |
| `session-reconnect` | Failure/recovery | 350 ms link loss, new session, new geometry | online recovery, generation 5, route restored, new road |
| `layout-rules` | Protocol/boundary | rev3 apply; rev2/rev3 replay; corrupted height; rev4 valid | `Duplicate`, `Failed`, rev4 applied (260 px / 2 components) |
| `geometry-mismatch` | Protocol/timing | generation 3 geometry, then generation 4 snapshot | generation 4 adopted, route points cleared |
| `geometry-recovery` | Failure/recovery | generation 4 geometry resent | route view restored for generation 4 |
| `wifi-errors` | Failure/boundary | scan, auth failure, timeout, success, disconnect, forget | state/error per step; final `NoCredentials` |
| `state-acquiring` | Functional state | no destination/fix | UI = `ACQUIRING_FIX` |
| `state-arrived` | Functional state | arrived + arrive maneuver | UI = `ARRIVED`, maneuver `ARRIVE` |
| `state-offline` | Functional state | navigating with network offline | UI = `OFFLINE`, `online=0` |
| `state-rerouting` | Functional state | rerouting | UI = `REROUTING` |
| `nav-end` | Functional state | navigation ends; the phone sends a terminal idle snapshot | link stays up, destination/route/distance/road name cleared, still online |
| `map-scene-stale` | Protocol/timing | rev7, then rev6, then rev8 | rev6 = `Duplicate`, rev8 adopted |
| `watchdog-offline` | Timing/failure | no heartbeats for 4 s | link inactive, UI `OFFLINE`, route cleared |
| `watchdog-recovery` | Failure/recovery | heartbeats and geometry after expiry | link active, online, route restored |
| `fragmentation` | Data boundary/protocol | 24 roads × 8 points + 16 buildings × 8 points; 8-component layout | maximum `MapScene` accepted, layout applied |

`--frames DIR` writes `01-offline-<id>.bmp` and `02-<id>.bmp`; inspect the final
frame at least once for UI changes.

## 4. Closure criteria

1. **State closure**: every `NavState` / `moto_ui_mode_t` has a reachable path
   and an assertion; every fault state has a case that returns to healthy.
2. **Timing closure**: normal, duplicate, out-of-order, timeout and stale inputs
   are covered by `ble_protocol_tests` and the simulator cases.
3. **Data closure**: legal minimum/maximum and illegal values are accepted or
   rejected, and the rejection is observable (ACK status).
4. **Cross-end closure**: the same golden fixture is recomputed by C++ and the
   Android JNI layer; Android and the simulator share one link format.
5. **Evidence closure**: every conclusion has command output or a frame; no
   "should be fine".

When one is missing, add a case rather than relax an assertion.

## 5. Layers

| Layer | Content | When |
| --- | --- | --- |
| L0 unit | coordinates, NavCore, protocol, Presenter, bridge, layout store | every change (`ctest`) |
| L1 simulator | the 15 cases above + frames | every change (`ctest`) |
| L2 Android | JVM unit tests, `assembleDebug`; emulator + `files/ble_sim` | Android changes |
| L3 web | WASM + browser scenario injection | UI/interaction changes |
| L4 hardware | RF, pairing, lock-screen liveness, panel, PSRAM/frame rate, IMU, power | release acceptance |

CI runs L0+L1 and builds the Android debug APK.

## 6. Adding cases

- one case focuses on one dimension; failure messages must state expected vs actual;
- assert observable state (`moto_ui_state_t`, ACK status, message fields), never internals;
- independent process, `--port 0`, bounded timeouts; no long sleeps;
- register the case in `selftest_phone.cpp`, `platforms/desktop/CMakeLists.txt`,
  the desktop README and `AGENTS.md`;
- protocol changes come first, then the case.

## 7. Hardware-only residual acceptance

BLE RF, real MTU negotiation, Just Works pairing, lock-screen/background
liveness; CO5300 panel colour/Gamma/TE and sustained frame rate; PSRAM pressure,
heap fragmentation, 40 Hz timing, power, temperature, vibration, water
resistance; QMI8658 heading fusion, WiFi RF association and weak-network
handover; real AMap data, cross-city routes, real-ride deviation.

Simulator results must never be cited as evidence for these items.
