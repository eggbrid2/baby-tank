# 2026-10-17 AI 行走与手机蓝牙遥控实测

## 已上线的控制链路

- 云端单次语音接口输出 TTS PCM，并可在响应头携带经过服务端校验的 `drive` 动作：`forward`、`backward`、`left`、`right`；时长由模型选择 2000–9000 ms，速度固定为当前实测可起步的 600‰。诊断调用、触摸事件和不符合明确祈使句的 ASR 文本不授权行走。
- 头部只接受版本 2、`execute/approved`、合法方向、600‰、2000–9000 ms 和 trace ID 完整的动作。完成 TTS 后，头部先取得底盘 STOP 精确回显，再按时长每不超过 80 ms 刷新运动命令，最后发 STOP 并核对回显。BLE 新命令或 STOP 可抢占 AI 动作。
- App 遥控使用 Espressif Security 1 加密 BLE 自定义端点，不需要先配 Wi-Fi。连接后自动发 STOP 并要求头部返回底盘 UART 精确回显，用户再手动启用摇杆。只提供已实测的前进、后退和原地左右转；松手、取消拖动或 App 进入后台发送 STOP。未收到新控制命令时头部约 300 ms 停车。
- 云端 `ai_action_log` 保留动作提议；`ai_action_execution` 记录授权与头部上报的 `completed/blocked/failed`。管理员接口把获授权记录显示为 `status=approved`，并保留 `proposal_status=shadow`；`executed=1` 仅指头部报告已完成并取得 STOP 回显，不是轮速传感器证据。

## 实机检查

- 手机：Android `25053RT47C`，ADB 序列号 `74c5f6ee`；新版 APK 安装成功。头部：ESP32-S3 `44:bd:8d:69:49:68`，0.13.6 仅刷 `ota_0` 应用分区 `0x100000`，写入哈希校验通过；NVS、模型和底盘 Flash 未改。旧头部应用分区备份：`artifacts/device-backups/20261017-head-pre-ai-drive/ota_0-6MiB.bin`，SHA-256 `5dc95fc599b6c64ed988aedcc01261178a69dd8ad626e18b764b2db916f839e5`。
- 头部启动后识别底盘、连接“泡面”网络并通过云端健康检查。App BLE 加密会话成功；头部日志显示 `chassis link verified by exact UART STOP echo`。
- 用户在履带架空条件下操作 App；日志分别记录左右转、前进、后退的 600‰ 命令，状态均为 0，松手 STOP 状态 0。出现一次 300 ms 控制命令超时，头部记录 `motion command expired; emergency stop`，之后仍可重新接收遥控。用户反馈“操控也可以了”；对履带物理表现的描述为“oj”，视为口头确认，不等同测量。
- 首次语音录音 1408 ms、上传 45056 PCM 字节，云端 ASR 未识别出文本，HTTP 422，头部未执行动作。第二轮录音 3488 ms、上传 111616 PCM 字节，云端 ASR 4935 ms、模型 625 ms、TTS 669 ms，HTTP 200；云端 TTS 总量 173092 字节、约 5409 ms 音频。头部收到 `forward/2000 ms`，随后记录 `result=completed reason=stop_echo_ok`；云端同一 trace ID `6557f9b3-be0f-4b01-aa98-43eb40009d59` 收到完成报告。用户对语音测试反馈“没问题”。
- 云端复部署后 `/healthz` 返回 `ok`；`npm test && npm run check`、头部 ESP-IDF 构建、控制协议宿主机测试、Android 构建与单元测试均通过。

## 仍须注意

- 头部 STOP 回显说明底盘收到相同 UART 文本，不提供编码器或电流反馈，不能单凭 `completed` 证明轮子实际行程。
- 底盘备份与上游源码中的计数器提示约 500 ms 独立失联停车，但尚未通过拔开头—底盘控制线做实物验证。落地负载、碰撞、电机堵转、供电压降、误识别概率和儿童长期使用安全未验证；目前仍应把此版本视为架空履带联调版。
- 首轮短语音产生 ASR 422；短促命令可能需要更清楚地说。成功轮头部播放缓存最低余量为 -346 ms，说明音频播放仍可能有供音卡顿，不能从云端 TTS 连续就推断设备端完全流畅。
- 头部上保留 USB 电机调试命令，量产前应通过构建开关禁用或移除。

## BLE 与 Wi-Fi 的选择

低速、低带宽、需要在未配网时仍可用的遥控适合 BLE；相机视频仍走局域网 Wi-Fi，不应搬到 BLE。ESP32-S3 的 BLE/Wi-Fi 共用 2.4 GHz 射频并进行时分共存，因此同时直播视频和遥控时仍须测最差延迟与停车行为。STA 加 BLE 是相对合适的常态；SoftAP 连接加 BLE 需要额外实测。参考 Espressif [共存指南](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32s3/api-guides/coexist.html)。
