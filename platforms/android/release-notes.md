社区 Android 移植**测试版（pre-release）**，非上游官方版本。

## 适配与来源

- 源码提交：`android-native-app` 分支最新提交
- 上游项目：https://github.com/mx3353672833-debug/moto-gps-waveshare
- 适配固件：仓库内现有 Waveshare ESP32-S3-Touch-AMOLED-1.75C 固件，未修改圆屏协议
- 许可证：沿用上游 PolyForm Noncommercial 1.0.0 与 NOTICE

## 已测设备与结果

- Xiaomi 14（Android 16）、Xiaomi Mi 10（Android 13）
- BLE：扫描 / 连接 / MTU 185→帧 182 / 三步握手 / 心跳 / 断连重连
- 导航：实时网关选路（GCJ-02）、路线几何 ACK=Ok、圆屏实际进入导航、地图场景 ACK
- 切页：导航 / 速度 / 指南针（手机罗盘航向）
- 音乐：检测到系统媒体会话（测试曲目「演员」），圆屏可遥控播放/切歌；设备不出声
- 熄屏：90 s 实测前台服务常驻、5 Hz 快照持续
- 离线地图：真实瓦片下载、清单、删除

## 未验证 / 限制

- 真实骑行与长时间锁屏耗电未实测
- 部分音乐 App 不暴露 MediaSession，圆屏无法控制
- 点赞按键在 Android 无通用接口，已回报不支持
- 圆屏中文字体仅含 105 个字形子集，生僻路名可能显示方框（固件字体问题，非协议）
- 未测试 Fake/自定义固件

## 安装

```bash
adb install motogps-0.1.0-alpha1.apk
```

与 debug 包签名不同，切换时需要先卸载 debug 包。

## 校验

SHA-256 见 Release 资产说明。
