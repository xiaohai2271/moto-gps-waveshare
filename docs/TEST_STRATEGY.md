> **语言 / Language:** 中文 · [English](TEST_STRATEGY.en.md)

# 测试策略：模拟优先的功能闭环

本文件定义这个项目的测试目标、维度模型、用例矩阵和“完成/交付”的判定标准。
核心原则：**能用宿主机模拟器验证的，不依赖真机；只有射频、面板和物理量留到硬件验收。**

## 1. 完成定义（Definition of Done）

一个功能或修复只有在同时满足以下条件时才视为完成：

1. 能在下面的**六维模型**中找到它影响的面；
2. 每个新增状态、错误分支和边界至少有一个可执行用例；
3. 本机 `ctest` 全绿（当前 26 项，含 16 个模拟器端到端用例）；
4. 协议字段变化时，golden fixture 与两端测试同步更新；
5. 交付说明包含：命令、退出码、`CASE PASS`/`ctest` 原文、截图路径、**未验证项**；
6. 明确列出仍需硬件的残余验收，不用模拟结果替代。

“编译通过”不算完成，“自检通过但没有断言”也不算完成。

## 2. 六维模型

| 维度 | 关注点 | 主要手段 |
| --- | --- | --- |
| 1. 功能状态 | Idle / Acquiring / Planning / Navigating / Rerouting / Arrived / Offline；导航、速度、指南针、音乐页 | 模拟器状态用例 + 宿主 `NavPresenter` 断言 |
| 2. 协议与时序 | 握手、session、心跳、看门狗、ACK/重试、重复、乱序、分片、陈旧代次 | 字节级用例 + `ble_protocol_tests` + Android `GoldenBytesJvmTest` |
| 3. 数据边界 | 坐标 E6、最大点数/道路/建筑/组件、字符串长度、枚举、空值、饱和 | 上限用例（fragmentation）+ 协议解码测试 |
| 4. 异常与恢复 | 断链、重连、新 session、几何代次不匹配、布局回滚/非法、WiFi 认证失败/超时、扫描与忘记网络 | 故障注入用例，验证“故障后下一帧恢复” |
| 5. 平台与交互 | ESP32 桥接、Android Compose/BLE/定位/网关、WASM Web、桌面模拟器；触摸/长按/滑页 | 模拟器、Android 单测/仪器测试、WASM 模拟器 |
| 6. 非功能 | 帧率、内存/PSRAM、功耗、温度、射频、面板颜色与 TE | 只能真机；模拟器提供帧与状态回归，不提供物理结论 |

## 3. 用例矩阵（当前自动化）

所有用例都是**独立进程**，入口为：

```sh
build/native/bin/moto_device_sim --case <id> --port 0
```

| ID | 维度 | 场景 | 关键断言 |
| --- | --- | --- | --- |
| `baseline` | 功能/协议/数据 | 握手 → 导航快照 → 3 块几何 → 地图 → 媒体 → 布局 → WiFi 成功/失败 | 模式、转向、距离、道路名、路线点、布局、在线状态 |
| `session-reconnect` | 异常/恢复 | 断链 350 ms，新 session 重连，重新下发几何 | 在线恢复、代次 5、路线点恢复、道路名更新 |
| `layout-rules` | 协议/边界 | rev3 应用；rev2/rev3 重放；篡改 `map_height=100`；rev4 合法 | rev2/3 = `Duplicate`；非法 = `Failed`；rev4 生效（260 px / 2 组件） |
| `layout-phone` | 数据/平台 | 按 Android 编辑器的 466 坐标下发 8 个组件（速度 160,312；剩余 120/270,390） | 坐标不被二次缩放、8 个组件全部应用、路线仍可见 |
| `geometry-mismatch` | 协议/时序 | 代次 3 几何完成后，收到代次 4 快照 | 采用代次 4，路线点清零，不残留旧几何 |
| `geometry-recovery` | 异常/恢复 | 代次 4 重新下发几何 | 路线点恢复且代次为 4 |
| `wifi-errors` | 异常/边界 | 扫描；认证失败；超时；成功；断开；忘记 | 每步状态/错误码；最终 `NoCredentials` |
| `state-acquiring` | 功能状态 | 无目的地、无定位 | UI = `ACQUIRING_FIX` |
| `state-arrived` | 功能状态 | Arrived + 到达动作 | UI = `ARRIVED`，动作为 `ARRIVE` |
| `state-offline` | 功能状态 | 导航中网络离线 | UI = `OFFLINE` 且 `online=0` |
| `state-rerouting` | 功能状态 | 重算中 | UI = `REROUTING` |
| `nav-end` | 功能状态 | 导航结束后手机发送终帧（Idle + GNSS 陈旧） | 链路保持、目的地/路线点/距离/道路名清零，仍在线 |
| `map-scene-stale` | 协议/时序 | rev7 后收到 rev6，再收到 rev8 | rev6 = `Duplicate`，rev8 生效 |
| `watchdog-offline` | 时序/异常 | 停止心跳 4 s | 链路 inactive、UI `OFFLINE`、路线点清零 |
| `watchdog-recovery` | 异常/恢复 | 断链后恢复心跳并重发快照/几何 | 链路 active、在线、路线恢复 |
| `fragmentation` | 数据边界/协议 | 24 道路×8 点 + 16 建筑×8 点；8 组件布局 | 最大 `MapScene` 通过，8 组件布局生效 |

辅助证据：`--frames DIR` 会输出 `01-offline-<id>.bmp` 与 `02-<id>.bmp`；
UI 变更应至少人工查看一次最终帧。

## 4. 闭环判定

功能测试闭环不是“用例数量”，而是下面五条同时成立：

1. **状态闭环**：每个 `NavState` / `moto_ui_mode_t` 都有可达路径和断言；
   故障状态（断链、离线、无凭据）之后存在回到正常状态的用例。
2. **时序闭环**：正常、重复、乱序、超时、陈旧五类输入都有覆盖；
   `Reassembler` / 握手 / 看门狗的边界由 `ble_protocol_tests` 与模拟器共同覆盖。
3. **数据闭环**：合法最小值、最大值、非法值都能被接受或拒绝，且拒绝原因可观察（ACK 状态）。
4. **跨端闭环**：协议字节用同一 golden fixture 在 C++、Android/JNI 两端复算；
   模拟器与 Android 走同一链路格式，不另造一套。
5. **证据闭环**：每个结论都有命令输出或帧文件支撑；没有“应该没问题”。

不满足时，优先补用例，而不是放宽断言。

## 5. 分层执行

| 层 | 内容 | 何时跑 |
| --- | --- | --- |
| L0 单元 | 坐标、NavCore、协议、Presenter、桥接、布局存储 | 每次改动，`ctest` |
| L1 模拟器 | 上表 16 个用例 + 帧导出 | 每次改动，`ctest` |
| L2 Android | JVM 单测、`assembleDebug`；模拟器 + `files/ble_sim` 连虚拟设备 | Android 改动 |
| L3 Web | WASM + 浏览器场景注入 | UI/交互改动 |
| L4 硬件 | 射频、配对、锁屏保活、面板、PSRAM/帧率、IMU、功耗 | 发布前残余验收 |

CI 当前执行 L0+L1，并构建 Android debug；L2 的模拟器 E2E 与 L4 由人工按清单执行。

## 6. 新增用例规则

- 一个用例只聚焦一个维度，失败信息必须指出“期望什么、实际什么”；
- 断言可观察状态（`moto_ui_state_t`、ACK 状态、消息字段），不断言内部实现；
- 用例独立进程、独立端口（`--port 0`）、独立超时，不依赖执行顺序；
- 允许的等待只有协议防抖（布局 2 s）与固定 100–350 ms 传播窗口，禁止长 sleep；
- 新增用例必须同时更新：`selftest_phone.cpp` 的用例表、`platforms/desktop/CMakeLists.txt`、
  `platforms/desktop/README.md` 和 `AGENTS.md`；
- 需要新消息/新字段时，先改 `shared/ble_protocol` 与 golden fixture，再改用例。

## 7. 仍然只能真机验证

- BLE 射频、真实 MTU 协商、Just Works 配对、锁屏/后台保活；
- CO5300 面板颜色/Gamma、TE 同步、持续帧率与撕裂；
- PSRAM 压力、堆碎片、40 Hz 时序、功耗、温度、振动、防水；
- QMI8658 航向融合、WiFi 射频关联与弱网切换；
- 真实高德数据、跨城路线、实车偏航与快速转向。

这些项目的验收记录不得引用模拟器结果。
