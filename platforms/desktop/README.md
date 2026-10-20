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

## 用例矩阵（CI 用，不需要 Android）

```sh
./build/native/bin/moto_device_sim --list-cases
./build/native/bin/moto_device_sim --case baseline --port 0 --frames build/native/frames
ctest --test-dir build/native --output-on-failure
```

每个用例都是独立进程、独立端口、独立断言；`ctest` 会逐项注册和报告。
测试策略与闭环判据见 [`docs/TEST_STRATEGY.md`](../../docs/TEST_STRATEGY.md)。

| 用例 | 覆盖维度 | 场景 |
| --- | --- | --- |
| `baseline` | 功能/协议/数据 | 握手、导航、几何、地图、媒体、布局、WiFi 成功/失败 |
| `session-reconnect` | 异常/恢复 | 断链后用新 session 重连并恢复几何 |
| `layout-rules` | 协议/边界 | 版本单调、重放 `Duplicate`、非法载荷 `Failed`、防抖 |
| `geometry-mismatch` | 协议/时序 | 新代次快照清除旧几何 |
| `geometry-recovery` | 异常/恢复 | 新代次几何恢复路线显示 |
| `wifi-errors` | 异常/边界 | 扫描、认证失败、超时、成功、断开、忘记 |
| `state-acquiring` | 功能状态 | 无目的地/无定位 → `ACQUIRING_FIX` |
| `state-arrived` | 功能状态 | 到达状态与动作 |
| `state-offline` | 功能状态 | 导航中离线 → `OFFLINE` |
| `fragmentation` | 数据边界 | 最大 `MapScene` + 8 组件布局的多帧路径 |

`--case baseline` 等价于原来的 `--selftest`。加 `--frames DIR` 会输出
`01-offline-<id>.bmp` 与 `02-<id>.bmp`；BMP 可直接用 Windows 照片查看，
需要 PNG 时用任意转换工具。

退出码 0 表示该用例通过；失败会打印 `CASE FAIL <id>: <原因>`，
加 `--verbose` 可看到固件侧日志。

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
| `src/phone_client.cpp` | 可复用的脚本手机（握手/快照/几何/布局/WiFi） |
| `src/selftest_phone.cpp` | 用例矩阵、脚本与 UI 断言 |
| `src/wifi_provisioning_host.cpp` | 可编排 WiFi 配网假实现 |
| `host_stubs/` | `board_port`、`esp_timer`、`esp_log`、`esp_err` |

## English summary

- `moto_device_sim` runs the real `PhoneNavBridge`, the shared `NavPresenter` and
  the shared LVGL UI on a headless 466×466 RGB565 display.
- The phone link is loopback TCP: an 8-byte header followed by one encoded v1 BLE
  frame. The transport reports an ATT MTU of 185 so frames are 182 bytes.
- `--case <id>` runs one deterministic end-to-end case in its own process;
  `--list-cases` prints the matrix and `ctest` runs all of them.
- Cases cover handshake/heartbeats, navigation, geometry generation mismatch and
  recovery, layout revision rules, WiFi failures, UI states, reconnection and
  maximum-size fragmentation.
- Debug Android builds attach through `TcpBleTransport` when `files/ble_sim`
  exists (`adb reverse tcp:9100 tcp:9100`).
- Not covered: BLE RF/pairing/background liveness, the panel, PSRAM pressure,
  the IMU, power, or real road routes.
