# Glimpse Moto GPS — Android（社区移植）

> 社区开发的 Android 配套 App，**非上游官方版本**。上游项目：
> https://github.com/mx3353672833-debug/moto-gps-waveshare
> 上游原创代码采用 [PolyForm Noncommercial 1.0.0](../../LICENSE.md)；本目录遵循同一许可证，
> 保留 [NOTICE](../../NOTICE) 与 [第三方说明](../../THIRD_PARTY_NOTICES.md)。
> 欢迎参与 Glimpse 共创：Issues / Pull Request 均可，实际贡献者会保留署名。

目标硬件：Waveshare ESP32-S3-Touch-AMOLED-1.75C（现有微雪固件，无需重刷、不改圆屏协议）。

## 架构

- **UI**：Kotlin + Jetpack Compose（Material 3），Activity 只负责挂载 Compose。
- **C++ 复用**：NDK + CMake + JNI 直接编译上游 `shared/ble_protocol`（帧/CRC/分片）与
  `shared/coordinates`（WGS84→GCJ-02），**不重写**这些逻辑；金标字节自检走同一份 C++。
- **分层**：UI → 导航会话 → 定位 / 网关 HTTP / BLE 传输 / 地图存储，逐层解耦。
- iOS 的 SwiftUI/CoreBluetooth/CoreLocation/ObjC++ 代码未复用；JNI 是新写的安卓适配层。

## 工具链（本机实际验证）

| 组件 | 版本 |
| --- | --- |
| JDK | Temurin 21.0.12.1+1 |
| Gradle | 9.5.0（wrapper 已提交） |
| AGP | 8.7.3 |
| Kotlin | 2.0.21 |
| compileSdk / targetSdk | 36 / 36 |
| minSdk | 26 |
| Build-Tools | 36.0.0 |
| NDK | 27.2.12479018 |
| CMake | 3.22.1 |
| ABI | arm64-v8a、x86_64 |

## 构建与测试

```powershell
# 仓库根目录，先拉子模块
git submodule update --init --recursive

cd platforms/android
# local.properties 只放本机 SDK 路径，已 gitignore
.\gradlew.bat assembleDebug          # app/build/outputs/apk/debug/app-debug.apk
.\gradlew.bat testDebugUnitTest      # JVM 单元测试
.\gradlew.bat connectedDebugAndroidTest   # 需已连接设备/模拟器：JNI 金标端到端
```

本机实测结果（2026-10-18，Windows 11 + Android 模拟器 forseti_p0 API 36 x86_64）：

| 检查 | 结果 |
| --- | --- |
| `assembleDebug` | BUILD SUCCESSFUL，APK 12.9 MB |
| `assembleDebugAndroidTest` | BUILD SUCCESSFUL |
| `testDebugUnitTest` | 6 个测试通过（包含独立 CRC 校验） |
| `connectedDebugAndroidTest` | 2 个测试通过：JNI 编码与上游 golden **逐字节一致**；坐标转换 |
| 模拟器安装 + App 内自检 | PASS，6/6 向量 match（截图 `docs/android-phase1-selfcheck.png`） |

![阶段一自检](../../docs/android-phase1-selfcheck.png)

## 接口依据（来自源码，不来自截图）

- Service `7e57a000-b50c-4b6a-9c57-40a54e8e1000`；手机写 `…a001`，订阅通知 `…a002`；
  CCCD `00002902-0000-1000-8000-00805f9b34fb`。
- 帧：magic `0xB7`、version 1、10 字节头 + payload + CRC-16/CCITT-FALSE（低字节先发）。
- 握手：Phone Starting → Device Ready → Phone Ready → Device Ready（session_id 必须一致）。
- 心跳 1000 ms，payload 为 `session_id + 会话内已用毫秒`（**不是**系统 uptime）。
- 网关：`GET /healthz`、`GET /v1/places`、`POST /v1/routes`、`POST /v1/route-options`、
  `GET /v1/map/cities`、`GET /v1/map/tiles/15/{x}/{y}`；请求 WGS84，路线/瓦片几何 GCJ-02。
- 文档与实现差异：ESP32 心跳失活阈值实为 `clamp(interval×3, 1500, 15000) ms`；
  `max_frame_size` 实际 clamp 到 [13, 512]；TrafficDeviation 段在 C++ 内部为 u32、线上为 u24。

## 开发进度

### 阶段一（工程、JNI、协议自检）— 已完成

| 项目 | 状态 |
| --- | --- |
| Gradle/Compose/NDK/CMake 工程，wrapper 提交 | 已实现 |
| JNI：crc16、fragmentMessage、decodeFrame、goldenVectors、wgs84ToGcj02 | 已实现并自动测试通过 |
| 上游 golden 字节端到端一致性 | 模拟器通过 |
| 协议常量与 fixture 结构 JVM 测试 | 通过 |
| CI：新增 android job（构建 + 单元测试 + 产物上传） | 已添加，待首次远端运行 |
| 真机 BLE 连接圆屏 | **尚未验证**（无真机） |

### 后续阶段

- 阶段二：BLE 扫描/连接/加密/订阅、三步握手、写泵（15 ms 单帧）、128 帧队列、
  心跳、ACK、退避重连；真机联调。
- 阶段三：网关客户端、定位（区分权限）、NavApp JNI 复用、真实导航与偏航重算。
- 阶段四：前台服务 + 锁屏导航、地图下载与缓存预算。
- 阶段五：预发布 APK、签名、SHA-256、GitHub Release。

## 权限说明

阶段一仅使用 `INTERNET`（后续阶段按需启用，已在 Manifest 声明但未在阶段一请求）：

- `BLUETOOTH_SCAN`（neverForLocation）/ `BLUETOOTH_CONNECT`：BLE 连接圆屏。
- `ACCESS_FINE_LOCATION` / `ACCESS_COARSE_LOCATION`：导航定位，与扫描权限分离。
- `FOREGROUND_SERVICE` + `LOCATION` / `CONNECTED_DEVICE`、`POST_NOTIFICATIONS`：阶段四。

## 当前限制

- BLE、锁屏导航、真实定位均未在真机验证；模拟器只证明编解码与 JNI 生命周期。
- `MOTO_BACKUP_DIR` 之类设备备份与本 App 无关；本 App 不上传轨迹或凭证。
- debug APK 为测试包，未签名发布。
