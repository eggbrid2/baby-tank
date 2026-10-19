# 摄像头区域无画面诊断（2026-10-19）

## 结论

摄像头传感器和固件视频服务当前正常，已确认的断点位于 Android App 的网络回退流程，而不是 GC2145 硬件。

App 会优先通过机器人所在家庭局域网访问 `http://<机器人地址>:8081/stream`。当手机不在同一子网或该路径不可用时，App 会通过 BLE 要求机器人恢复 SoftAP，但成功回调只进入 `showManualAp()`，没有调用已经存在的 `BabyTankVideoClient.connect(deviceName, setupCode)`。因此 App 不会发起 `WifiNetworkSpecifier` 请求，也不会自动把视频 socket 绑定到机器人热点，摄像头区域会持续无画面，直至用户手动切换系统 Wi-Fi 后再次点击。

## 实机证据

2026-10-19 通过头部 USB Serial/JTAG `/dev/cu.usbmodem2121101` 读取当前启动日志：

- 实机运行 `baby_tank_head 0.13.32`，不是 `agent-readme.md` 尚未更新的 `0.13.20`。
- GC2145 PID `0x2145` 被正确识别。
- 84,480 B RGB565 帧缓冲在 PSRAM 分配成功。
- 优化 JPEG 编码器初始化成功。
- SoftAP HTTP 服务绑定到 AP 接口，局域网 HTTP 服务绑定到 STA 接口。
- 固件报告 `GC2145 RGB565/JPEG camera ready`。
- 机器人家庭网地址为 `172.16.209.223`；从同网电脑请求 `http://172.16.209.223:8081/health` 得到预期的 `403 camera token required`，证明服务可达且鉴权正在工作。
- 云健康检查通过后，固件在约 19.2 秒切换为 `WIFI_MODE_STA` 并记录 `internet verified; setup hotspot stopped`。这是当前按需热点策略，不代表摄像头停止；局域网 8081 服务仍在。

## 源码证据

- 修复前的 `android/app/src/main/java/com/babytank/app/MainActivity.kt:145-160`：先读局域网地址；不在同一子网时要求固件启动热点；热点启动成功后仅调用 `videoClient.showManualAp()`。
- `android/app/src/main/java/com/babytank/app/wifi/BabyTankVideoClient.kt:55-129`：自动连接 SoftAP 的完整 `WifiNetworkSpecifier` 实现已经存在，但修复前没有任何调用方。
- `android/app/src/main/java/com/babytank/app/MainActivity.kt:62,88`：App 已内置并用于 BLE 会话的 `BuildConfig.BABY_TANK_SETUP_POP`，因此具备推导 SoftAP 密码所需的数据，不需要再次要求用户输入配对码。
- `firmware/head/main/network_runtime.c:391-405`：云端验证成功 10 秒后关闭初始 SoftAP。
- `firmware/head/main/network_runtime.c:136-148`：BLE 摄像头命令可重新开启 SoftAP。

## 修复方案

1. 保留局域网 8081 快速路径。
2. 局域网路径不可用且 `enableCameraHotspot()` 成功后，直接调用 `videoClient.connect(device.name, BuildConfig.BABY_TANK_SETUP_POP.toCharArray())`，触发 Android 系统的一次 Wi-Fi 授权并将视频 socket 绑定到该网络。
3. 不建议让 SoftAP 永久常开。Wi-Fi 与 BLE 共用 2.4 GHz 射频，永久 APSTA 会增加功耗和共存干扰；更完整的产品方案应增加“关闭摄像头热点”BLE 命令或租约超时，在退出遥控页后恢复 STA 常态。
4. 去掉 `showManualAp()` 中硬编码的 `BabyTank-694968`，至少改为使用当前设备名，避免后续更换硬件时误导用户。
5. 增加 Android 测试，覆盖“LAN 可达”“LAN 不可达后自动请求 SoftAP”“用户拒绝系统 Wi-Fi 授权”“视频断流重连”四条路径。

## 修复

用户连接手机后，已将热点启动成功回调从 `videoClient.showManualAp()` 改为
`videoClient.connect(device.name, BuildConfig.BABY_TANK_SETUP_POP.toCharArray())`。
App 版本同步提升为 `0.1.4 (5)`。固件未修改。

首次安装复测又确认了一个更靠前的 UI 状态耦合：控制页只在
`controlReady`（底盘 STOP 回显验证成功）时自动连接摄像头，并且画面区域在
`controlReady == false` 时只允许重新验证底盘。摄像头硬件与底盘验证没有依赖关系，
因此已改为以 BLE 设备 `connected` 作为摄像头启动条件；底盘不可用时只禁用行走和
底盘相关功能，不再阻止视频。

## 验证结果

- `./gradlew testDebugUnitTest assembleDebug` 构建成功；项目当前没有 Android 单元测试源码，Gradle 明确报告 `NO-SOURCE`，因此不能把该任务当成单元测试覆盖证明。
- 修复包已通过 ADB 安装到手机 `74c5f6ee`，系统确认版本为 `0.1.4 (5)`。
- App 自动恢复 BLE 安全会话后进入横屏遥控页；即使底盘仍显示“准备中”，摄像头区域已经显示真实 GC2145 画面，语义树出现 `宝宝视角实时画面`，摄像头按钮变为“关画面”。
- App 进程日志未出现视频 HTTP、解码、连接超时或崩溃错误。
- 安装包已更新到 `artifacts/android/BabyTank-debug.apk`，SHA-256：`99e302ebf4ee6dc3d742686e965954542d99434aa24d9fdd458a21b777542bc7`。
- 固件没有修改或烧录；本次故障由 Android 状态耦合与 SoftAP 回退断点共同造成。
