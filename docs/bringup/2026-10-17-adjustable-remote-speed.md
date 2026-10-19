# Android 遥控速度条与 AI 控车边界记录

日期：2026-10-17

## 结论

- 原来手动遥控由 App 和头部固件共同硬限制在 600‰（60%）。上游底盘解析 `x/y` 浮点指令并以 `MOTOR_SPEED_MAX=100` 计算 PWM，因此协议的有效上限是 1000‰（100%），不是 60%。
- 横屏遥控页新增速度条：40%-100%、每 10% 一档、默认 100%。前进、后退、原地左转和原地右转统一使用当前档位。
- 头部仍拒绝范围外数值，并保留精确 UART 回显、300 ms deadman、松手 STOP 和失联停车。大于 100% 的 UI 档位没有物理意义，底盘最终会在 100% 截止。
- AI 行走仍固定为 600‰，手机速度条只影响 BLE 手动遥控。AI 与手动速度不共享状态。
- 速度条位于中央摄像头画面下方的独立控制带，不再覆盖画面。

## 遥控模式与 AI

进入横屏遥控页会取得遥控独占租约，WakeNet、语音对话、触摸 AI 和 AI 行走会暂停。这是此前“手机遥控时停止一切 AI”的明确产品规则，不是速度条引入的回归。正常退出立即请求恢复；如果 BLE 在横竖屏切换中断开，固件最多 5 秒后因租约过期恢复 WakeNet。

AI 行走还需要同时满足：

1. ASR 得到明确、独立的行走祈使句；
2. 云端安全规则返回 `execute/approved`；模型仅可补充安全时长和灯光，不能否决明确指令；
4. 头部收到完整 trace ID、600‰ 和 2000-9000 ms；
5. 底盘 STOP 精确回显成功。

因此“前进”“向前走一下”“左转 3 秒”可以授权；“为什么不能前进”、假设、引用和否定句不会授权。触摸事件也无权触发行走。

## 修改位置

- `firmware/head/main/chassis_bridge.c`：手动控制绝对上限从 ±600‰ 调整为 ±1000‰。
- `android/app/src/main/java/com/babytank/app/MainActivity.kt`：把速度条幅值映射到四方向履带命令。
- `android/app/src/main/java/com/babytank/app/ble/BabyTankProvisioner.kt`：BLE 客户端范围调整为 ±1000‰。
- `android/app/src/main/java/com/babytank/app/ui/BabyTankApp.kt`：摄像头下方加入独立离散速度条；改变档位完成时先发 STOP。

## 验证状态

- Android `0.1.3`（versionCode 4）`assembleDebug` 通过。
- Android 单元测试任务通过，但当前工程没有 Android 单元测试源码，Gradle 报告 `NO-SOURCE`；这不能替代实机 UI 和运动测试。
- ESP-IDF 6.1 构建通过，应用镜像剩余 59%。
- 控制协议宿主机测试通过：±1000‰ 接受、1001‰ 拒绝。
- AI/云端核心测试 `node --test test/*.test.mjs`：9/9 通过，包含明确行走意图授权和间接语句拒绝。
- 完整 `npm test` 已限定到正式测试目录并通过 9/9；`npm run check` 通过。

## 产物与未验证项

- 待烧录头部镜像：`firmware/head/build/baby_tank_head.bin`
  - SHA-256：`dec3f4a745785579bf37155f008073025d91862d9a8727d08e406c99fa7f3303`
- 待安装 APK：`artifacts/android/BabyTank-debug.apk`
  - SHA-256：`2ff2b1c657a2735c4276ce39b99c0e5d21157d8845d21408ce87909880e450a7`
- 头部已通过 `/dev/cu.usbmodem311301` 仅写应用分区并完成 Flash 哈希校验；启动日志确认 `baby_tank_head 0.13.20`、97.5% 左履带直行补偿、WakeNet、摄像头以及云端健康检查正常。
- APK 已通过 ADB 安装到 `74c5f6ee`；系统包信息确认 `versionName=0.1.3`、`versionCode=4`。
- 安装和启动成功不等于 70%-100% 已完成运动验收；本次没有通过自动化坐标发送运动命令，以免横屏坐标映射误触导致小车突然移动。
- 已知事实仅为 60% 在架空履带测试中可工作；100% 的落地速度、电池压降、电机温升、履带打滑、转向稳定性和儿童碰撞风险仍需现场验证。
