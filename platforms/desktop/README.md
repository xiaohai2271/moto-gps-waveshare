# Host virtual device（无硬件联调）

`moto_device_sim` 在电脑上运行**真实的固件代码**：`PhoneNavBridge`、共享
`NavPresenter`、共享 LVGL 界面和 `shared/ble_protocol`。它用一个 headless
466×466 RGB565 显示器和一条 loopback TCP 链路，替代屏幕和 BLE 射频。

它的目的不是替代真机验收，而是把开发过程中除射频/面板之外的环节全部搬到电脑上：

- UI 改动立刻可见，并可导出 BMP 快照做回归对比；
- BLE 握手、分片、ACK、心跳、重连、断链时序用真实 C++ 跑；
- 导航/几何/路况/媒体/布局/WiFi 配网消息走真实编解码；
- Android debug 包可以直接连这个虚拟设备，不需要 ESP32。

## 构建

Linux / macOS：

```sh
cmake -S . -B build/native -DMOTO_BUILD_TESTS=ON -DMOTO_BUILD_SIM=ON
cmake --build build/native --parallel
```

Windows（在 VS Developer PowerShell 中，NMake 生成器）：

```powershell
cmake -S . -B build/native -G "NMake Makefiles" `
      -DCMAKE_BUILD_TYPE=Release -DMOTO_BUILD_TESTS=ON -DMOTO_BUILD_SIM=ON
cmake --build build/native
```

> 本仓库在受限沙箱/部分环境中使用 Ninja 会在 `try_compile` 阶段挂起，因此
> Windows 文档默认 NMake；普通开发机 Ninja 也可用。

产物固定写到 `build/native/bin/moto_device_sim`（Windows 为 `.exe`）。

## 自检（CI 用，不需要 Android）

```sh
./build/native/bin/moto_device_sim --selftest --frames build/native/frames
```

自检会在后台启动一个"脚本手机"，通过 TCP 走完整条链路：

1. `Phone Starting → Device Ready → Phone Ready → Device Ready` 三步握手；
2. 心跳、`UiLayoutState`、`WifiStatus`；
3. `NavigationSnapshot`（右转 120 m、泉城广场北路、72 km/h）；
4. 3 个 `RouteGeometry` 分块 + 下一帧快照；
5. `MapScene`、`MediaState`、`UiLayout`（含 2 s 防抖后的 `UiLayoutState`）；
6. WiFi 扫描、连接成功、认证失败；
7. 断言真实 LVGL 状态（模式、转向、道路名、路线点、布局）与帧缓冲变化。

退出码 0 表示通过。加 `--frames` 会输出：

- `01-offline.bmp`：未连接状态；
- `02-navigation.bmp`：收到导航快照后的圆屏画面。

BMP 可直接用 Windows 照片查看；需要 PNG 时用任意转换工具。

## 作为虚拟设备运行

```sh
./build/native/bin/moto_device_sim --port 9100
```

它会打印监听的地址。加 `--verbose` 可以看到固件日志（或设置
`MOTO_SIM_VERBOSE=1`）；`--seconds N` 用于限时冒烟。

## 连接 Android（debug 包）

前提：Android debug 包已安装，虚拟设备已在本机 9100 端口运行。

```sh
adb reverse tcp:9100 tcp:9100
adb shell run-as dev.glimpse.motogps \
    sh -c 'echo 127.0.0.1:9100 > files/ble_sim'
# 重新打开 App；日志 tag 为 MotoGpsSim
```

`files/ble_sim` 是开关文件：

- 空文件 = 默认 `127.0.0.1:9100`；
- 内容为 `host:port` 时使用指定地址（例如模拟器访问宿主机可写 `10.0.2.2:9100`）；
- 删除文件 = 恢复真实 `BluetoothGatt` 传输。

```sh
adb shell run-as dev.glimpse.motogps rm files/ble_sim
```

App 内仍是真实的 `Esp32BleCentral`（握手机、队列、重连、ACK），只把
`BleTransport` 换成了 `TcpBleTransport`；ATT MTU 固定报告 185，因此分片帧长与
真机一致（182 字节）。

## 链路格式

一条消息 = 一个完整编码后的 BLE 帧，前面加 8 字节小头：

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 4 | magic `"MOTO"`（`4D 4F 54 4F`） |
| 4 | 1 | 版本 `1` |
| 5 | 1 | 类型 `1` = BLE frame |
| 6 | 2 | payload 长度，大端 |
| 8 | N | 一个 `ble_protocol` 帧 |

实现见 `src/link_protocol.h` 与 Android 的
`platforms/android/app/src/debug/java/dev/glimpse/motogps/ble/TcpBleTransport.kt`。

## 覆盖范围

| 已验证 | 说明 |
| --- | --- |
| 握手/心跳/ACK/重连 | 真实 `VirtualBleTransport` 状态机 + 真实 `PhoneNavBridge` |
| 分片与重组 | 与固件相同 `max_frame_size=182`，多分块消息 |
| 导航 UI | 共享 LVGL、共享字体、真实 466×466 RGB565 |
| 布局下发 | 2 s 防抖、应用、`UiLayoutState` 回报 |
| WiFi 配网 | 可编排假实现：成功、认证失败、扫描列表 |
| Android 全链路 | debug 注入 + 真实 `Esp32BleCentral` |

## 未覆盖（必须真机）

- BLE 射频、真实 MTU 协商、Just Works 配对、锁屏后台保活；
- CO5300 面板颜色/Gamma、TE 同步、持续帧率；
- PSRAM/堆压力、40 Hz 时序、功耗与温度；
- QMI8658 航向融合、WiFi 射频关联；
- 真实高德数据、实车偏航与跨城路线。

这些边界与 `docs/TESTING.md`、`docs/ANDROID_AI_GUIDE.md` 的口径一致：
模拟器通过不等于硬件验收通过。

## 代码结构

| 文件 | 作用 |
| --- | --- |
| `src/main.cpp` | CLI 与 40 Hz 主循环 |
| `src/device_sim.cpp` | 组装 presenter/bridge/transport/server，驱动帧 |
| `src/virtual_ble_transport.cpp` | NimBLE 传输层的宿主机替身（握手/ACK/心跳/看门狗） |
| `src/headless_display.cpp` | 466×466 RGB565 显示与 BMP 导出 |
| `src/tcp_link.cpp` | 服务端/客户端与链路分帧 |
| `src/selftest_phone.cpp` | 脚本手机与端到端断言 |
| `src/wifi_provisioning_host.cpp` | 可编排 WiFi 配网假实现 |
| `host_stubs/` | `board_port`、`esp_timer`、`esp_log`、`esp_err` |

## English summary

- `moto_device_sim` runs the real `PhoneNavBridge`, the shared `NavPresenter` and
  the shared LVGL UI on a headless 466×466 RGB565 display.
- The phone link is loopback TCP: an 8-byte header followed by one encoded v1 BLE
  frame. The transport reports an ATT MTU of 185 so frames are 182 bytes.
- `--selftest` drives the handshake, heartbeats, navigation, geometry, map scene,
  media, layout and WiFi, then asserts the device UI state and a changed frame.
- Debug Android builds attach through `TcpBleTransport` when `files/ble_sim`
  exists (`adb reverse tcp:9100 tcp:9100`).
- Not covered: BLE RF/pairing/background liveness, the panel, PSRAM pressure,
  the IMU, power, or real road routes.
