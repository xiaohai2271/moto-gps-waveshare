# 圆屏导航布局编辑与设备设置方案

> 状态：讨论稿 v0.1（2026-10-19）
> 范围：Android App + ESP32-S3 固件；iOS 不在本期。
> 关联：`shared/protocol/ble-navigation-v1.md`、`shared/nav_ui/`、`shared/nav_presenter/`、`platforms/android/`
>
> 本文是设计基线，不是已实现功能的说明。评审通过后再拆任务实施。

## 0. 一页结论

- **布局以手机为唯一编辑源**：App 本地保存布局，连接时与改动后通过新增 BLE `UiLayout` 消息下发；设备原子应用并写入 NVS，断连/重启后沿用最后一次布局。
- **可编辑组件 8 个**：实时速度、限速牌、当前方向、转向指示、剩余距离、剩余时间、导航进度弧、速度/加速度弧。地图区支持高度与透明度调整；车标固定（水平居中）；页签固定。
- **编辑方式**：466×466 圆形预览内自由拖拽 + 8 px 网格/中线吸附；圆弧组件不自由拖拽，只能停靠上/下/左/右四条边，弧长 90°/120°/180° 可选。
- **每个组件**：显示/隐藏、S/M/L 尺寸、透明度 10%–100%。
- **方向模式**：导航页全局“北向上 / 设备向上”，默认**北向上**；航向取手机 `heading_cdeg`，设备不做罗盘/IMU 航向解算；北向上时地图不转、车标按手机航向旋转。
- **底图 v1 只做矢量主题**：主题/配色/线宽、路线与街道各自透明度、缩放级别、图层显隐。栅格影像底图按你的决定**本期砍掉，不提供入口/控件**。
- **音乐卡片**：不新增导航页卡片；只增加“独立音乐页显示/隐藏”开关（固件 API 已存在，目前由媒体能力位强制打开，需要改为用户开关）。
- **权限中心**：Android；列出定位、蓝牙、通知、媒体通知使用权，进入页面即检查，缺失项提供申请或系统设置跳转。
- **WiFi**：真实配网。手机热点（手动或 Android LocalOnlyHotspot 自动模式），BLE 新增 `WifiConfig`/`WifiStatus`/`WifiScan` 消息，设备保存 NVS 并连接；v1 只做配网、连接与状态展示，数据面（瓦片/OTA）后续。

## 1. 现状与约束

### 1.1 设备

- Waveshare ESP32-S3-Touch-AMOLED-1.75C：466×466 圆屏、32 MB flash、8 MB octal PSRAM、LVGL。
- 固件分区：8 MB app + 7 MB SPIFFS storage，NVS 24 KB（`platforms/esp32/partitions.csv`）。
- 现有固件没有 WiFi 初始化；BLE 使用 NimBLE peripheral，ATT MTU 185。

### 1.2 数据链路

- 手机负责定位、路线规划、偏航重算，ESP32 只接收显示投影（`ble-navigation-v1.md` §1）。
- 现有消息：`NavigationSnapshot`（≤5 Hz）、`RouteGeometry`（≤24 点）、`TrafficDeviation`、`MediaState`、`MapScene`（≤24 条道路/≤16 个建筑）、`DeviceCommand`。
- `MapScene` 是 **GCJ-02 矢量道路/建筑**；gateway 的 `/v1/map/tiles` 返回 JSON 矢量文档，不是栅格图。设备没有瓦片金字塔、没有 SD 卡。
- 帧协议 v1：10 字节头 + 分片 + CRC16；`max_frame_size` 在 MTU 185 下为 182。

### 1.3 现有导航页

`shared/nav_ui/src/moto_nav_ui.cpp` 在 `create_navigation_page()` 中写死坐标：

- 地图层（道路、建筑、路线、车标）
- 顶部状态文字
- 转向图标 + 距转向距离（固定在图标右侧）
- 限速牌
- 进度弧（316×316，84°，居中）
- 页签圆点
- 全屏连接/等待覆盖层

速度、指南针、音乐是独立页面；导航页目前**不显示**实时速度、当前航向、剩余距离、剩余时间。

### 1.4 现有设置与权限

- Android 首次启动申请定位、蓝牙、通知（`MainActivity.requestMissingPermissions()`），没有状态页，也没有媒体通知使用权检查。
- 设备详情页只有连接/断开、扫描结果、定位状态。
- 音乐页开关固件 API 已存在（`moto_nav_ui_set_music_page_enabled`），但 `PhoneNavBridge` 目前按媒体能力位强制开关，用户无法控制。
- 固件无 WiFi 配网，无本地 HTTP 服务。

### 1.5 协议与能力位

- `ConnectionStatus.capabilities` 的 bit0–bit7 已用满。
- 未知消息类型在固件侧会被 `decode_message` 拒绝，`PhoneNavBridge` 丢弃并回 `Failed` ACK，**不会断开连接**（已核对 `phone_nav_bridge.cpp:499` 与 `ble_nav_transport_nimble.cpp:471`）。
- 因此新增可选消息类型可以保持帧协议 v1，只需新增能力位。

## 2. 已确认的产品决策

| 编号 | 决策 | 说明 |
| --- | --- | --- |
| D1 | 只做 Android + 固件 | iOS 本期不同步；共享代码仍需保持可编译 |
| D2 | 像素自由拖拽 + 吸附 | 圆形安全区内任意位置；网格/中线/边缘吸附 |
| D3 | 尺寸 S/M/L、每组件单实例 | 不允许同一组件多份 |
| D4 | 圆弧贴四边 | 上/下/左/右四个锚点，不自由拖拽 |
| D5 | 弧长 90/120/180° 可选 | 每个弧独立配置 |
| D6 | 音乐只控制独立音乐页显隐 | 不在导航页新增卡片 |
| D7 | 权限中心含媒体通知使用权 | 列表展示状态 + 快捷跳转 |
| D8 | WiFi 真实配网 | 手机热点（手动/自动 LocalOnlyHotspot） |
| D9 | 本期砍掉栅格影像底图 | 只做矢量主题；不提供影像入口 |
| D10 | 离线可编辑、连上同步、导航中实时生效 | 手机本地为准，设备 NVS 兜底 |

> D9 的解释：你在“瓦片来源”里同时选了“真实栅格影像”和“砍掉栅格”。本方案按**本期不实现栅格、也不保留影像入口**执行，避免出现不可用的死控件；后续要加时再按 §P5 单独评审。

## 3. 需求映射

| 需求 | 方案 | 阶段 |
| --- | --- | --- |
| 1.a 北向上/设备向上 | 默认北向上；`UiLayout.flags.north_up`；航向取手机；Presenter 旋转策略 + 车标反向旋转 | P1 |
| 1.b 限速/实时速度/当前方向 | 组件 `speed_limit`、`speed`、`heading`，自由拖拽 | P1 |
| 1.c 进度弧、四方向、可挪动 | 组件 `progress_arc`，四锚点，弧长可选 | P1 |
| 1.d 转向指示 | 组件 `maneuver`（图标 + 距转向距离） | P1 |
| 1.e 底图/路线/街道样式 | 矢量主题、配色、线宽、透明度、缩放、图层显隐；栅格延后 | P2（栅格 P5/暂缓） |
| 1.f 速度/加速度弧 | 组件 `speed_arc`，四锚点，弧长可选 | P1 |
| 1.g 剩余距离/时间 | 组件 `remaining_distance`、`remaining_time` | P1 |
| 1.h 组件透明度 | 每个组件 `opacity` 10–100% | P1 |
| 2 权限检查/跳转 | Android 权限中心 | P3 |
| 3 音乐卡片显隐 | 独立音乐页开关，随布局下发 | P1 |
| 4 WiFi 配置 | BLE 配网 + 设备 WiFi STA + App 配网页 | P4 |

## 4. 布局模型

### 4.1 坐标系与安全区

- 画布固定 466×466，原点左上，中心 `(233, 233)`。
- 圆形安全区半径 220（留 13 px 边距）。组件外接矩形必须落在圆内；拖拽时钳制，越界回弹。
- 组件坐标用**外接矩形左上角** `(x, y)`，配合固定档位尺寸，App 与固件不需要测量文本即可复现位置。
- 页签圆点、连接覆盖层、状态文字不参与布局，保持固定。

### 4.2 组件清单

| ID | key | 显示内容 | 数据来源 | 拖动 | 默认 |
| ---: | --- | --- | --- | --- | --- |
| 1 | `speed` | 实时速度数值 + km/h | `speed_deci_kph` | 自由 | 开，M |
| 2 | `speed_limit` | 圆形限速牌 | `speed_limit_kph`（0 时隐藏） | 自由 | 开，M |
| 3 | `heading` | 航向角度 + 方位字 | `heading_cdeg` | 自由 | 关 |
| 4 | `maneuver` | 转向图标 + 距转向距离 | `maneuver` + `distance_to_maneuver_m` | 自由 | 开，M |
| 5 | `remaining_distance` | 剩余距离 | `remaining_distance_m` | 自由 | 开，M |
| 6 | `remaining_time` | 剩余时间 | `remaining_duration_s` | 自由 | 开，M |
| 7 | `progress_arc` | 已行驶/未行驶弧 | `route_progress_percent` | 仅四边 | 开，上边，120° |
| 8 | `speed_arc` | 速度弧 + 加速度读数 | `speed_kph` + `accel_mps2` | 仅四边 | 开，下边，120° |

固定元素：地图区（只调高度/透明度）、车标、页签圆点、连接覆盖层、顶部状态文字。

说明：

- `maneuver` 把图标和“300 m / 1.2 km”合并为一个组件，保持现有“距离紧贴图标”的语义；不做两个独立组件。
- `speed_limit` 在 `speed_limit_kph == 0` 时自动隐藏，不占布局位置。
- `heading` 默认关闭，避免与转向指示重复；需要时打开。
- `remaining_distance/time` 只在有路线时显示；无路线时自动隐藏。

### 4.3 尺寸档位

所有尺寸为 466 画布下的设计像素（px 宏会按实际画布缩放）。

| 组件 | S | M | L |
| --- | --- | --- | --- |
| `speed` | 72×56 | 96×72 | 124×92 |
| `speed_limit` | 44×44 | 61×61 | 78×78 |
| `heading` | 68×60 | 88×76 | 112×96 |
| `maneuver` | 150×108 | 180×128 | 210×148 |
| `remaining_distance` | 92×44 | 116×56 | 146×68 |
| `remaining_time` | 78×44 | 100×56 | 128×68 |
| `progress_arc` | 弧宽 4 | 弧宽 6 | 弧宽 8 |
| `speed_arc` | 弧宽 4 | 弧宽 6 | 弧宽 8 |

- 文字字号尽量复用现有资源（Montserrat 16/20/28/48、`moto_font_nav_16/20/28`），避免新增字库占用 flash。
- 圆弧半径固定贴边：`progress_arc` 直径 440，`speed_arc` 直径 440；当两个弧停在同一侧时，`speed_arc` 自动缩到 408，保证内外分离。
- 地图高度连续可调 160–466 px，默认 232；透明度 10–100%。

### 4.4 拖拽、吸附与约束

- 拖拽以组件中心为手柄，实时更新预览。
- 吸附优先级：中线（x=233 / y=233，阈值 6 px）> 8 px 网格 > 自由位置。
- 圆弧组件拖拽时显示四个锚点槽；松手吸附到最近锚点（阈值 24 px），不允许停在非锚点位置。
- 钳制规则：组件外接矩形四角必须在半径 220 的圆内；越界时沿最近方向回弹。
- 组件之间不做碰撞检测，允许重叠（由用户负责）；圆弧同侧自动内外分层。

### 4.5 圆弧规则

- 画布中心 `(233, 233)`，LVGL 角度约定：0° = 右侧（3 点钟），顺时针增加。
- 锚点中心角：上 270°、右 0°、下 90°、左 180°。
- 弧段范围：`start = center - span/2`，`end = center + span/2`，`span ∈ {90, 120, 180}`。
- `progress_arc`：底弧表示未行驶，指示弧表示已行驶，从 `start` 顺时针增长；指示弧颜色沿用交通色（畅通/缓行/拥堵/严重）。
- `speed_arc`：指示弧按 0–160 km/h 填充；弧色随加速度变化（加速偏绿、匀速冰蓝、制动偏红），弧内侧显示带符号加速度读数（如 `+1.2`、`-0.8`，单位 m/s²）。
- 透明度同时对底弧和指示弧生效（`arc_opa`），数值文本单独按组件透明度处理。

### 4.6 透明度

- 每个组件独立 `opacity`，范围 10–100，编码 0–100（步进 5）。
- 文字/图标用 `lv_obj_set_style_opa`；圆弧用 `arc_opa`；地图底图与路线分层各自应用。
- 低于 10% 不允许，避免“组件消失但开关仍开着”的困惑；需要完全隐藏请用显示开关。

### 4.7 方向模式

- `north_up = 1`（默认，北向上）：地图固定北向、不旋转；车标按手机航向旋转。
- `north_up = 0`（设备向上）：地图按手机航向旋转，车标固定朝上。
- 航向唯一来源是手机 `heading_cdeg`；设备不运行磁力计/IMU 航向解算，也不把 IMU 航向写入显示。固件在 40 Hz 运动帧里对手机航向做最短角平滑（避免 5 Hz 更新造成跳变）。
- 方向模式是导航页全局设置，不影响速度/指南针/音乐独立页；指南针页同样使用手机航向。
- 切换时手机立即下发新布局，设备下一帧生效；不重建页面。

### 4.8 地图区

- 只支持高度与透明度调整，不支持自由拖动；地图始终贴顶。
- 高度变化会改变投影可视区域：Presenter 的车标中心 y 按地图高度等比调整（默认 `196 / 232 ≈ 84.5%`），保持前方视野比例；路线窗口按新高度裁剪。
- 缩放：`zoom_percent` 50–200，100 = 现有 `0.44 px/m`。手机端按 `radius_m = clamp(500 × 100 / zoom_percent, 100, 1500)` 调整 MapScene 窗口；缩放到 50% 时窗口约 1 km，需要按现有 192 点上限做抽稀。
- 图层开关：道路、建筑、路线。关闭后设备隐藏对应对象；手机可后续优化为不再发送对应数据。

### 4.9 布局 JSON 示例（App 本地存储）

```json
{
  "schema": 1,
  "revision": 12,
  "orientation": "heading_up",
  "music_page_enabled": true,
  "map": { "height_px": 232, "opacity": 100, "zoom_percent": 100 },
  "basemap": {
    "theme": "dark",
    "route":   { "color": "#FFFFFF", "width_px": 6, "opacity": 100 },
    "street":  { "color": "#8E8E93", "width_px": 3, "opacity": 35 },
    "building":{ "opacity": 25 },
    "layers":  { "roads": true, "buildings": true, "route": true }
  },
  "components": {
    "speed":              { "enabled": true,  "x": 185, "y": 300, "size": "M", "opacity": 100 },
    "speed_limit":        { "enabled": true,  "x": 300, "y": 238, "size": "M", "opacity": 100 },
    "heading":            { "enabled": false, "x": 24,  "y": 40,  "size": "M", "opacity": 100 },
    "maneuver":           { "enabled": true,  "x": 29,  "y": 220, "size": "M", "opacity": 100 },
    "remaining_distance": { "enabled": true,  "x": 150, "y": 390, "size": "M", "opacity": 90 },
    "remaining_time":     { "enabled": true,  "x": 250, "y": 390, "size": "M", "opacity": 90 },
    "progress_arc":       { "enabled": true,  "anchor": "top",    "span_deg": 120, "opacity": 80 },
    "speed_arc":          { "enabled": true,  "anchor": "bottom", "span_deg": 120, "opacity": 80 }
  }
}
```

### 4.10 默认布局

- 默认布局 = 当前导航页的视觉基线 + 新组件最小集合：`maneuver`、`speed_limit`、`speed`、`remaining_distance`、`remaining_time`、`progress_arc`(上,120°)、`speed_arc`(下,120°)。
- `heading` 默认关闭。
- 默认布局同时写入固件默认值和 App 首次安装值；提供“恢复默认布局”按钮，二次确认后下发。
- 默认坐标是占位值，P1 实现时在真机上调一版；调好后固件与 App 常量同步，避免预览偏差。

## 5. BLE 协议扩展

### 5.1 原则

- 帧协议保持 v1；只新增消息类型和能力位，不改现有字段顺序/单位/枚举。
- 所有新消息第 0 字节为 `payload_revision = 1`。
- 布局消息带 `ACK_REQUESTED`；设备在完整校验、应用、NVS 写入成功后才回 `Ack(Ok)`。
- 手机只在双方能力位都包含对应位时发送；未知消息在旧固件上被安全丢弃。

### 5.2 能力位

| bit | 名称 | 方向 | 含义 |
| ---: | --- | --- | --- |
| 8 | `CapabilityUiLayout` | 双向 | 支持 `UiLayout` / `UiLayoutState` |
| 9 | `CapabilityWifiProvisioning` | 双向 | 支持 WiFi 配网消息 |

固件 `ConnectionStatus` 的 capability 增加 bit8；P4 再增加 bit9。手机端同步。

### 5.3 `UiLayout`（0x15，手机→设备）

固定 22 字节 + 每组件 10 字节，最多 102 字节；建议 `ACK_REQUESTED`，不加 `URGENT`，避免与导航帧抢发送队列。

```text
u8  revision              固定 1
u16 layout_revision       手机侧单调递增，仅用于同步/去重
u8  flags                 bit0 north_up, bit1 music_page_enabled, bit2 map_visible
u8  theme_id              0 dark, 1 light, 2 high_contrast, 3 custom
u16 map_height_px         160..466，偶数
u8  map_opacity           0..100
u8  route_r, route_g, route_b     0..255
u8  route_width_px        1..16
u8  route_opacity         0..100
u8  street_r, street_g, street_b  0..255
u8  street_width_px       1..12
u8  street_opacity        0..100
u8  building_opacity      0..100
u8  layer_mask            bit0 道路, bit1 建筑, bit2 路线
u8  zoom_percent          50..200
u8  component_count       0..8
repeat component_count:
  u8  id                  1..8（见 §4.2）
  u8  cflags              bit0 enabled
  u8  size                0 S, 1 M, 2 L
  u8  opacity             0..100
  i16 x                   左上角；圆弧为 0
  i16 y
  u8  anchor              0 上, 1 右, 2 下, 3 左；非圆弧为 0
  u8  span_deg            90/120/180；非圆弧为 0
```

校验规则：

- `component_count` 超限、ID 重复/未知、`size > 2`、`opacity > 100`、`anchor > 3`、`span_deg` 非 90/120/180、非圆弧组件的 `anchor/span_deg` 非 0、坐标越界（非圆弧）→ 整条拒绝，回 `Ack(Failed)`，屏幕不变。
- 非圆弧组件的 `x/y` 必须落在 466×466 画布内且外接矩形不出安全圆；圆弧组件的 `x/y` 必须为 0。
- 同一侧出现两个圆弧时，固件按 `progress_arc` 外、`speed_arc` 内应用，不视为错误。
- 未知组件 ID：整条拒绝（v1 不做部分应用），便于 App 明确知道固件不支持。

### 5.4 `UiLayoutState`（0x16，设备→手机）

设备在握手 Ready 后发一次，应用布局成功后也可再发；8 字节。

```text
u8  revision              固定 1
u16 layout_revision       当前生效版本
u8  source                0 default, 1 nvs, 2 phone
u8  supported_mask        bit0..bit7 对应组件 ID 1..8
u8  flags                 bit0 nvs_saved
u16 reserved              固定 0
```

手机收到后：

- `layout_revision` 与本地相同 → 跳过推送；
- 不同 → 推送当前布局；
- `supported_mask` 缺位 → 对应组件在编辑器中置灰并提示“固件不支持”。

### 5.5 WiFi 配网消息（P4）

`WifiConfig`（0x17，手机→设备，ACK_REQUESTED）：

```text
u8  revision
u8  action              0 保存并连接, 1 临时连接, 2 断开, 3 忘记
u8  flags               bit0 隐藏 SSID, bit1 自动重连
u8  ssid_len            0..32
u8  pass_len            0..64
u8  ssid[]              UTF-8，无 NUL
u8  pass[]              UTF-8，无 NUL
```

`WifiStatus`（0x18，设备→手机）：

```text
u8  revision
u8  state               0 off, 1 connecting, 2 connected, 3 failed, 4 no_credentials
i8  rssi_dbm
u8  error               0 none, 1 auth, 2 ssid_not_found, 3 timeout, 4 internal
u32 ipv4_le
u8  ssid_len
u8  ssid[]
```

`WifiScanRequest`（0x19，手机→设备）：`revision, u16 request_id`。

`WifiScanResult`（0x1A，设备→手机）：

```text
u8  revision
u16 request_id
u8  count               0..16
repeat:
  i8  rssi_dbm
  u8  auth              0 open, 1 wpa2, 2 wpa3, 3 wpa2/wpa3
  u8  ssid_len
  u8  ssid[]
```

安全边界：

- 密码不写日志；设备 NVS 明文保存（与现有项目一致），后续可评估 NVS 加密；
- 只支持 WPA2/WPA3 个人版，不做企业认证；
- 扫描期间允许 BLE 导航继续，但 WiFi 扫描会短暂占用射频，固件需限制扫描频率。

### 5.6 时序与去重

- 手机：布局变更后 300 ms 防抖，合并为最新一版发送；同一 `layout_revision` 不重复发。
- 设备：按 `layout_revision` 去重；重复收到同版本直接回 `Ack(Duplicate)`，不重复写 NVS。
- ACK 超时重发沿用现有策略（750 ms，最多 2 次）；仍失败则 App 标记“未同步”，下次连接重试。
- NVS 写入防磨损：应用成功后延迟 2 s 落盘；2 s 内又收到新版本则只落最终版。断电最多丢最后一次调整，不丢已确认版本。

### 5.7 兼容与降级

| 组合 | 行为 |
| --- | --- |
| 新 App + 新固件 | 完整功能 |
| 新 App + 旧固件 | 能力位缺 bit8：隐藏“圆屏布局”入口或显示“固件需升级”，不发送 0x15 |
| 旧 App + 新固件 | 固件使用 NVS/默认布局；旧 App 不感知 |
| 布局消息校验失败 | 整条拒绝，保留当前画面，回 `Failed` |
| WiFi 能力缺失 | App 显示“固件不支持 WiFi 配网” |

### 5.8 黄金帧与测试

- 在 `shared/protocol/fixtures/ble-navigation-v1.golden.txt` 增加 `UiLayout` 的完整 payload 与分片示例。
- 共享 codec 测试覆盖：全组件往返、边界值（0/100 透明度、90/180 弧长、S/L 尺寸）、未知 ID、重复 ID、越界坐标、trailing bytes。
- 旧固件行为用单元测试锁定：未知类型返回 `UnknownMessageType`，不改变连接状态。

## 6. 固件设计

### 6.1 模块划分

| 模块 | 改动 |
| --- | --- |
| `shared/ble_protocol` | 新增 `UiLayout`/`UiLayoutState`/WiFi 结构体、编解码、消息类型、能力位、golden |
| `platforms/esp32/main/ble_nav_transport_nimble.cpp` | 能力位声明；WiFi 消息转发；不改分片/重组 |
| `platforms/esp32/main/phone_nav_bridge.{h,cpp}` | 接收布局 → 校验 → 持久化 → 应用（同时配置 Presenter 与 NavUI）；WiFi 状态回调 |
| `shared/nav_presenter` | 方向模式、地图高度、缩放参数 |
| `shared/nav_ui` | `moto_ui_layout_t`、新组件对象、`apply_layout`、圆弧重定位、透明度、车标旋转、音乐页开关 |
| 新增 `platforms/esp32/main/ui_layout_store.{h,cpp}` | NVS 读写、默认布局、版本比较 |
| 新增 `platforms/esp32/main/wifi_provisioning.{h,cpp}` | WiFi STA、扫描、NVS、状态机、可选 HTTP 健康检查 |

### 6.2 渲染层重构

- 新增 `moto_nav_ui_apply_layout(const moto_ui_layout_t *)`，只做“应用”，不重建对象；布局切换应在一帧内完成。
- 在 `shared/nav_ui/include/moto_nav_ui.h` 新增 C 兼容的 `moto_ui_layout_t`（组件数组固定 8 项）；`PhoneNavBridge` 负责 `ble::UiLayout` → `moto_ui_layout_t` 映射，UI 层不直接依赖 BLE 头文件。
- 创建顺序保持：先创建全部组件（隐藏），再由布局决定位置/尺寸/透明度/显隐。
- 现有 `update_navigation()` 会对 `nav_distance/nav_unit` 每帧执行绝对 `lv_obj_align`（`moto_nav_ui.cpp:1416`）；布局化后改为相对 `maneuver` 组件内部定位，否则每帧都会覆盖拖拽结果。
- 最终可见性 = 布局开关 && 数据可用：无路线时 `maneuver`、剩余距离、剩余时间自动隐藏；`speed_limit_kph == 0` 时限速牌自动隐藏。
- 音乐页最终可见性 = 布局开关 && 媒体能力位；`PhoneNavBridge` 不再用 `music_page_enabled_ = media` 直接覆盖用户开关。
- 地图层拆分：
  - `nav_map_base`：道路 + 建筑（应用 `map_opacity` 与图层开关）；
  - `nav_route_layer`：路线 + 阴影（应用 `route_opacity`，独立于底图）；
  - `nav_marker`：车标（不受底图透明度影响）；
  - 现有代码中路线是 `nav_map` 的子对象，需要提升为兄弟层，避免调底图透明度时路线一起变淡。
- 圆弧：
  - `lv_arc_set_bg_angles` + `lv_arc_set_rotation` 按锚点/弧长设置；
  - 进度弧和速度弧各自独立对象，同侧时内径按 §4.3 调整；
  - 透明度用 `arc_opa`，数值文本用对象 `opa`。
- 尺寸档位：根据档位切换字体（复用现有 Montserrat / `moto_font_nav_*`）和对象宽高；`maneuver` 图标按档位缩放绘制。
- 车标旋转：仅在 `north_up` 时按航向旋转；旋转中心为对象中心，使用 `transform_rotation`，避免每帧重绘路径。

### 6.3 状态与投影

- `moto_ui_state_t` 不新增字段，保持 iOS/Web 适配器不变；布局配置由 `PhoneNavBridge` 持有，分别配置 Presenter 与 NavUI。
- `NavPresenter` 增加 `set_view_config(north_up, map_height_px, zoom_percent)`：
  - `north_up` 时投影航向取 0；
  - 车标中心 y = `map_height_px × 196 / 232`（默认 232 时保持现有 196）；
  - 缩放系数作用于 `kMapPixelsPerMeter`；
  - 路线/道路/建筑裁剪到地图高度。
- `moto_nav_ui_apply_layout(const moto_ui_layout_t *)` 保存 `north_up` 供车标旋转使用，并应用其余视觉属性。
- 方向切换不重建页面，只重投影 + 车标旋转，保证 40 Hz 运动帧不中断。

### 6.4 加速度

- 不扩展 `NavigationSnapshot`，在 `nav_ui` 内计算，避免污染共享导航模型：
  - `update_navigation()` 每次收到新的 `speed_kph` 时计算 `a = Δv / Δt`，`Δt` 取自 `lv_tick_get()`；
  - 1.5 s 滑动窗口 + EMA 平滑，避免 GPS 速度噪声；
  - 饱和 ±20 m/s²，静止/丢星时归零；
  - 滤波逻辑拆成纯函数 `shared/nav_ui/src/accel_filter.h`，便于 host 单测。
- `speed_arc` 的颜色阈值：`a > +0.5` 加速（绿），`|a| ≤ 0.5` 匀速（冰蓝），`a < -0.5` 制动（红）。
- 不使用 IMU 加速度：GPS 速度差分的物理含义更接近“车辆加速度”，且不受安装角度影响。

### 6.5 持久化

- NVS namespace `ui_cfg`，key `layout` 存完整二进制 payload，key `rev` 存版本；另存 CRC32 或长度校验。
- 启动顺序：LVGL/UI 创建 → NVS 读取 → `apply_layout`；若在 UI 创建前读取，先缓存在 `PhoneNavBridge`，创建完成后立即应用。校验失败/缺失 → 默认布局。
- 布局版本与 App 不一致时，以手机推送为准；设备不主动回滚。
- 恢复出厂/擦除 NVS → 默认布局。
- 音乐页开关、方向模式、底图样式都包含在布局里，统一持久化。

### 6.6 WiFi

- `esp_wifi` STA 模式 + `esp_event` + `esp_netif`；NVS namespace `wifi_cfg` 保存 SSID/密码/flags。
- 状态机：`off → connecting → connected/failed`；失败原因映射到 `WifiStatus.error`。
- 自动重连：`auto_reconnect=1` 时断线重试（指数退避 1/2/4/8 s，上限 30 s）；否则保持断开。
- 扫描：`esp_wifi_scan_start`，最多返回 16 个 AP，按 RSSI 降序；扫描时暂停自动重连。
- BLE/WiFi 共存：ESP32-S3 单射频，开启 coexistence，WiFi 省电模式 `WIFI_PS_MIN_MODEM`；布局/WiFi 消息优先级低于导航快照。
- v1 数据面：仅提供 `GET /health` 本地 HTTP 端点用于联调（返回 `{"ok":true,"fw":"..."}`），不传瓦片、不做 OTA。
- 安全：不记录密码日志；配网消息只在加密 BLE 连接上传输。

### 6.7 资源与性能

- WiFi 栈预计增加 ~600 KB flash、~50 KB RAM；32 MB flash / 8 MB PSRAM 足够，但需实测 LVGL 帧率与堆余量。
- 布局应用只在版本变化时执行，不在每帧执行；透明度/位置设置是 O(组件数)。
- 圆弧重设角度会触发重绘，布局切换时允许一帧抖动。
- `UiLayout` 最大 102 字节，单帧（MTU 185）即可发完，不增加分片压力。

## 7. Android 设计

### 7.1 页面结构

- 设备详情页新增入口：
  - “圆屏布局” → `LayoutEditorScreen`
  - “权限检查” → `PermissionCenterScreen`
  - “圆屏 WiFi” → `WifiSettingsScreen`（P4）
- 布局编辑器为全屏页面，顶部 466×466 圆形预览，底部三个页签：组件 / 底图 / 全局。
- 编辑中不阻塞导航；导航中进入编辑器时顶部显示“导航进行中，修改会实时生效”。

### 7.2 编辑器交互

- 预览按手机宽度缩放（留边距），保持 1:1 比例。
- 点选组件 → 高亮外框 + 底部属性面板：
  - 显示开关
  - 尺寸 S/M/L
  - 透明度滑杆 10–100
  - 圆弧额外：锚点四选一、弧长 90/120/180
- 拖拽：`pointerInput` + `detectDragGestures`；拖动时实时更新预览，松手后吸附并触发保存。
- 工具栏：撤销/重做（本地栈）、恢复默认、预览演示数据开关。
- 圆弧组件在预览四边显示四个锚点槽；拖到槽附近高亮。
- 组件列表页签显示全部 8 个组件及开关，便于关闭遮挡项。
- 底图页签：主题预设、路线/街道颜色与线宽、透明度、缩放、图层开关（不出现影像/来源入口）。
- 全局页签：方向模式、音乐页开关、地图高度/透明度。

### 7.3 预览实现

- 新增 `DevicePreview` Composable，用 Compose Canvas 绘制 466 画布：
  - 圆形裁切、安全区参考线；
  - 组件按同一套尺寸/位置/透明度绘制；
  - 圆弧用 `drawArc`，角度换算与固件一致；
  - 演示数据：速度 72、限速 70、航向 359°、右转 300 m、剩余 8.2 km / 21 min、进度 24%、加速度 +1.2。
- 预览是“近似复刻”，不是设备帧缓冲镜像；几何与透明度必须一致，字体渲染允许有差异。
- 为降低偏差：尺寸表、默认布局、圆弧角度公式在 App 与固件各保留一份常量，并在文档 §4 锁定；P1 完成后用真机截图对比校准一次。

### 7.4 数据层与同步

- `layout/LayoutModels.kt`：数据类 + 默认布局 + JSON 序列化。
- `layout/LayoutStore.kt`：DataStore 持久化；`revision` 自增。
- `layout/LayoutCodec.kt`：调用共享 C++ 的 JNI 编码（新增 `NativeBridge.encodeUiLayout`），不在 Kotlin 里重复实现字节格式。
- `NavViewModel` 新增：
  - `layout: StateFlow<UiLayout>`
  - `updateComponent(...)` / `updateBasemap(...)` / `updateGlobal(...)`
  - `resetLayout()`
  - `pushLayout()`（连接时自动调用）
- `Esp32BleCentral`：
  - 握手 Ready 后检查能力位 bit8；
  - 收到 `UiLayoutState` 比较版本，决定是否推送；
  - 发送 `UiLayout` 走现有 OutboundQueue，带 ACK 重试；
  - 收到 ACK 后更新“已同步/未同步”状态。
- 离线编辑：DataStore 立即保存；未连接时标记“待同步”；连接后自动推送。

### 7.5 权限中心

- 入口：设备详情页“权限检查”；首页/设备详情在缺失关键权限时显示轻量提示（可选）。
- 列表项：
  1. 定位（精确）— `ACCESS_FINE_LOCATION`；缺失时申请；仅模糊定位时提示“导航精度受限”。
  2. 蓝牙（附近设备）— API 31+ `BLUETOOTH_SCAN/CONNECT`；缺失时申请，或跳蓝牙设置。
  3. 通知 — API 33+ `POST_NOTIFICATIONS`；缺失时申请，或跳应用通知设置。
  4. 媒体通知使用权 — `NotificationListenerService`；只能跳 `Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS`。
- 每项显示：图标、名称、状态（已授权/未授权/受限/未开启）、操作按钮。
- 顶部汇总“3/4 已就绪”；进入页面自动刷新，`onResume` 再刷新。
- 运行时权限用 `RequestMultiplePermissions`；特殊权限用 Intent 跳转，返回后刷新。
- 不使用“全部申请”一键按钮：通知使用权没有运行时弹窗，混在一起会误导。
- 清单文件补充（P4 需要）：`NEARBY_WIFI_DEVICES`（API 33+，`neverForLocation`）、`CHANGE_WIFI_STATE`、`ACCESS_WIFI_STATE`。

### 7.6 WiFi 配网（P4）

- 页面顶部模式选择：
  - **自动热点（推荐）**：App 启动 LocalOnlyHotspot，读取 SSID/密码，自动下发 `WifiConfig`；设备连接后显示 IP/RSSI。
  - **手动热点**：用户输入自己热点的 SSID/密码；适用于系统限制 LocalOnlyHotspot 的机型。
  - **扫描**：向设备发 `WifiScanRequest`，列表选择 SSID 后输入密码。
- LocalOnlyHotspot 要点：
  - API 26+；需要定位权限且系统定位开关打开；
  - API 30+ 尽量指定 2.4 GHz 频段（ESP32-S3 不支持 5 GHz）；
  - 页面退出/切后台时关闭 reservation，避免热点常驻耗电；
  - 失败时回退手动模式，并给出明确原因。
- 状态卡：设备 SSID、IP、RSSI、错误原因；提供断开/忘记。
- 安全：手动输入的密码只存本地 DataStore（后续可换 EncryptedSharedPreferences）并随配网消息发送；日志脱敏。
- v1 联调验收：手机能访问 `http://<device-ip>/health`。

### 7.7 自检与调试入口

- 复用 `SelfCheckScreen` 增加：
  - 布局推送/ACK 次数、最近版本；
  - WiFi 状态；
  - 权限汇总。
- Debug 构建增加“强制推送默认布局”“模拟旧固件（忽略 bit8）”开关，便于回归。

## 8. 分阶段实施

估算为单人熟悉代码后的工作量，仅用于排期参考。

### P1 布局协议 + 固件布局引擎 + Android 编辑器（核心）

范围：

- 共享协议：`UiLayout`/`UiLayoutState`、能力位、编解码、golden、JNI。
- 固件：新组件对象、`apply_layout`、尺寸/透明度/显隐、圆弧四锚点与三档弧长、方向模式、地图高度/透明度、音乐页开关、NVS。
- Android：布局模型/存储/编码、编辑器预览、拖拽吸附、属性面板、同步与 ACK 状态、恢复默认。
- 不包含：底图配色/缩放/图层（P2）、权限（P3）、WiFi（P4）。

验收：

- 8 个组件可独立显示/隐藏、S/M/L、透明度 10–100；
- 圆弧四边 × 三档弧长全部正确；
- 北向上/设备向上切换，车标与地图行为正确；
- 布局重启/断连后保留；
- 旧固件不崩溃、不回显错误状态；
- 导航中推送布局不丢帧（连续 10 分钟骑行日志无 watchdog/丢帧告警）。

预计：8–12 人日。

### P2 底图矢量样式

范围：

- 主题预设（深色/浅色/高对比）、路线/街道自定义配色与线宽、路线/街道/建筑透明度；
- 缩放 50–200% 与 MapScene 窗口联动；
- 图层显隐；
- 不提供栅格影像/来源入口。

验收：

- 主题切换在 1 帧内完成；
- 缩放到 50%/200% 时路线与道路对齐，无明显错位；
- 关闭建筑/道路后对应对象消失，导航仍可用。

预计：3–5 人日。

### P3 权限中心

范围：权限列表、状态检测、运行时申请、系统设置跳转、返回刷新；媒体通知使用权深链。

验收：

- 四项权限状态准确；
- 拒绝后不崩溃，页面给出可操作提示；
- 系统设置返回后状态自动刷新。

预计：2–3 人日。

### P4 WiFi 配网

范围：固件 WiFi STA/扫描/NVS/状态；BLE 配网消息；Android 配网页、LocalOnlyHotspot、手动模式、扫描列表；`/health` 联调端点。

验收：

- 自动热点与手动热点均能连上；
- 设备重启后自动重连；
- 错误原因（密码错/找不到 SSID/超时）正确展示；
- BLE 导航与 WiFi 同时工作 10 分钟无断连。

预计：4–6 人日。

### P5 栅格影像底图（本期不做）

保留扩展点：手机渲染 466×466（或半分辨率）JPEG 帧，经 WiFi 本地 HTTP 传给设备，设备解码后作为 `nav_map_base` 贴图；路线/车标仍矢量。需要新增瓦片源与授权，单独评审。

## 9. 测试计划

### 9.1 共享协议

- `UiLayout` 全组件往返、边界值、非法输入、trailing bytes；
- 帧分片在 20/182/512 字节下的重组；
- 未知消息类型不改变连接状态；
- golden fixture 字节级比对。

### 9.2 固件

- 布局解析/校验/默认回退单元测试（host 构建）；
- 圆弧角度计算、组件钳制、尺寸映射的纯函数测试；
- NVS 读写、掉电模拟（写入中途重启）后的回退；
- 真机：透明度/弧长/方向/音乐页开关逐项核对；堆余量与帧率记录。

### 9.3 Android

- 钳制/吸附/圆弧角度纯函数单元测试；
- 布局 JSON 序列化与默认值测试；
- 权限状态映射与 Intent 目标测试（Robolectric）；
- 编辑器 Compose UI 测试：拖动、吸附、属性修改、撤销/恢复默认；
- 截图测试：默认布局、四锚点圆弧、透明度极值。

### 9.4 硬件联调

- 连接后自动同步；修改后 300 ms 内下发并 ACK；
- 导航中连续修改 20 次，无断连/重启；
- 设备重启、手机重启、双方重装后的布局一致性；
- 旧固件 + 新 App 的降级路径；
- WiFi 自动/手动热点、错误的密码、超出范围、重连。

## 10. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| LVGL 透明度 + 圆弧重绘开销 | 布局切换掉帧 | 只在版本变化时应用；真机测帧率；必要时合并重绘 |
| 地图高度/缩放改动投影 | 路线错位、车标偏移 | Presenter 参数化并加 host 测试；保留旧常量作为 100% 基线 |
| 预览与真机视觉偏差 | 用户排好版但设备不一致 | 共用尺寸表/角度公式；P1 结束做一次真机截图校准 |
| 圆弧同侧重叠 | 视觉拥挤 | 自动内外分层；编辑器提示“同侧已占用” |
| NVS 频繁写入 | flash 磨损 | 2 s 防抖落盘；相同版本去重 |
| WiFi + BLE 共存 | 吞吐下降、断连 | coexistence + 省电模式；配网消息低优先级；10 分钟压测 |
| LocalOnlyHotspot 机型差异 | 自动模式不可用 | 手动热点兜底；明确错误提示；2.4 GHz 频段约束 |
| 旧固件不识别新消息 | 用户以为编辑无效 | 能力位门控 + “固件需升级”提示；未知消息安全丢弃已锁定 |
| 媒体通知使用权无法程序化申请 | 权限中心体验受限 | 明确“去设置”按钮与返回刷新；不假装能一键授权 |

## 11. 待定项

1. **默认布局的最终坐标**：P1 真机调版后回填 §4.10。
2. **加速度显示格式**：当前设计为 `+1.2 m/s²`；若你更想要 `+4 km/h/s` 或只保留颜色趋势，评审时改。
3. **编辑器是否提供“导航中只读预览”**：当前设计允许实时编辑；若担心骑行误触，可加二次确认。
4. **布局多套预设/分享**：本期不做；后续版本再加 `preset_id` 与导入导出。
5. **底图主题的具体色板**：P2 出三套预设后确认。
6. **WiFi 数据面用途**：栅格瓦片、OTA、在线路况三选一或组合，P4 后单独排期。
7. **iOS 跟进时间**：共享协议与布局模型已按跨端设计，iOS 侧主要是 SwiftUI 编辑器与权限页。

## 12. 变更记录

| 日期 | 版本 | 变更 |
| --- | --- | --- |
| 2026-10-19 | v0.1 | 首版：需求澄清 + 布局模型 + 协议草案 + 分阶段计划 |
