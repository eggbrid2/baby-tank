# Baby Tank 文档索引

这里保存产品架构、设备协议以及按日期记录的硬件联调结论。文档中的“已实现”“已构建”和“已实机验证”含义不同；每份记录应明确证据和遗留项，不能用源码存在代替实机结论。

## 必读文档

1. [`../README.md`](../README.md)：产品定位、项目分类、设计思路、玩法和快速开始。
2. [`../agent-readme.md`](../agent-readme.md)：硬件事实、上游项目边界、许可证风险、安全要求和长期上下文。
3. [`architecture.md`](architecture.md)：云端组件、信任边界、OTA、AI/ASR/TTS 适配。
4. [`device-api.md`](device-api.md)：设备身份、对话、心情、OTA 和控制接口。
5. [`../firmware/head/README.md`](../firmware/head/README.md)：头部固件能力、接口约束、相机和构建方法。
6. [`../android/README.md`](../android/README.md)：Android 功能、传输边界、相机流程和遥控安全门。

## 产品与角色

- [`bringup/2026-10-16-baby-persona-touch-wake.md`](bringup/2026-10-16-baby-persona-touch-wake.md)：宝宝人格、触摸和本地唤醒。
- [`bringup/2026-10-19-mood-standby-touch.md`](bringup/2026-10-19-mood-standby-touch.md)：慢心情、短时情绪、正常/无聊/睡觉状态和梦境短句。
- [`bringup/2026-10-16-ai-face-control-contract.md`](bringup/2026-10-16-ai-face-control-contract.md)：AI 到表情系统的白名单边界。
- [`bringup/2026-10-19-interaction-games-and-generated-art.md`](bringup/2026-10-19-interaction-games-and-generated-art.md)：喂食、戳一戳、小游戏和儿童视觉资产。

## 表情与屏幕

- [`bringup/2026-10-16-generated-minimal-face.md`](bringup/2026-10-16-generated-minimal-face.md)：最初形象生成与资产转化。
- [`bringup/2026-10-16-pet-face.md`](bringup/2026-10-16-pet-face.md)：宠物脸基线。
- [`bringup/2026-10-16-procedural-line-face-animation.md`](bringup/2026-10-16-procedural-line-face-animation.md)：程序化线条表情。
- [`bringup/2026-10-16-premium-manga-face-animation.md`](bringup/2026-10-16-premium-manga-face-animation.md)：漫画感动画与灵动性设计。
- [`bringup/2026-10-19-svg-drawing-pipeline.md`](bringup/2026-10-19-svg-drawing-pipeline.md)：白名单 SVG 绘画管线。
- [`bringup/2026-10-16-lcd-hello-world.md`](bringup/2026-10-16-lcd-hello-world.md)：屏幕最初点亮记录。

## AI、语音与动作

- [`bringup/2026-10-16-ai-dialogue-expression-bridge.md`](bringup/2026-10-16-ai-dialogue-expression-bridge.md)：对话状态、表情时间线和动作桥接。
- [`bringup/2026-10-16-conversational-pcm-runtime.md`](bringup/2026-10-16-conversational-pcm-runtime.md)：PCM 语音运行时。
- [`bringup/2026-10-16-local-wakenet-plan.md`](bringup/2026-10-16-local-wakenet-plan.md)：本地 WakeNet 路线和唤醒词选择。
- [`bringup/2026-10-17-ai-drive-shadow-log.md`](bringup/2026-10-17-ai-drive-shadow-log.md)：AI 行走提案的影子日志阶段。
- [`bringup/2026-10-17-ai-drive-and-ble-control.md`](bringup/2026-10-17-ai-drive-and-ble-control.md)：AI 行走与 BLE 抢占。
- [`bringup/2026-10-17-ai-exploration-and-expression.md`](bringup/2026-10-17-ai-exploration-and-expression.md)：摄像头探索和表达联动。
- [`bringup/2026-10-17-ai-action-group-latency-steering.md`](bringup/2026-10-17-ai-action-group-latency-steering.md)：TTS、表情、灯光、摄像头和底盘并行动作组。

## Android、遥控与视频

- [`bringup/2026-10-16-android-ble-provisioning-control.md`](bringup/2026-10-16-android-ble-provisioning-control.md)：Android BLE 配网和控制基础。
- [`bringup/2026-10-16-softap-camera-video.md`](bringup/2026-10-16-softap-camera-video.md)：机器人 SoftAP 与 MJPEG 视频。
- [`bringup/2026-10-17-adjustable-remote-speed.md`](bringup/2026-10-17-adjustable-remote-speed.md)：遥控速度设置。
- [`bringup/2026-10-17-remote-mode-and-cannon-animation.md`](bringup/2026-10-17-remote-mode-and-cannon-animation.md)：遥控租约、AI 隔离和开炮动画。
- [`bringup/2026-10-17-camera-fps-and-control-sounds.md`](bringup/2026-10-17-camera-fps-and-control-sounds.md)：视频帧率与控制音效。
- [`bringup/2026-10-17-camera-memory-and-stream-test.md`](bringup/2026-10-17-camera-memory-and-stream-test.md)：摄像头内存和出流测试。
- [`bringup/2026-10-19-camera-region-diagnosis.md`](bringup/2026-10-19-camera-region-diagnosis.md)：摄像头区域故障诊断。
- [`bringup/2026-10-16-personal-app-touch-ai.md`](bringup/2026-10-16-personal-app-touch-ai.md)：个人 App、触摸与 AI 联动。

## 硬件、内存与固件迁移

- [`bringup/2026-10-16-original-firmware-identification.md`](bringup/2026-10-16-original-firmware-identification.md)：原设备固件和分区识别。
- [`bringup/2026-10-16-independent-firmware-cutover.md`](bringup/2026-10-16-independent-firmware-cutover.md)：切换到自研固件的依据和步骤。
- [`bringup/2026-10-17-chassis-wired-uart-test.md`](bringup/2026-10-17-chassis-wired-uart-test.md)：头部到底盘 UART 实机测试。
- [`bringup/2026-10-19-runtime-memory-modes.md`](bringup/2026-10-19-runtime-memory-modes.md)：SRAM/PSRAM 占用与互斥运行模式。

## 记录规范

新增联调记录放入 `docs/bringup/YYYY-MM-DD-topic.md`，至少包含：

- 目标和问题前提；
- 已核实事实与仍属推测的部分；
- 代码、硬件和云端分别改了什么；
- 构建、自动化测试和实机测试证据；
- 未验证项、风险以及可回滚方式；
- 相关固件/App 版本和提交号。

不要在文档中写入设备令牌、Wi-Fi 密码、云端密钥、私钥路径内容或可直接复用的登录凭据。
