# Baby Tank

Baby Tank 是面向 ESP-SparkBot 硬件重新开发的桌面宠物项目，包含自研 ESP32-S3 头部固件、Android 控制 App 和轻量云端服务。上游 ESP-SparkBot 工程只用于核对硬件和协议，不是本项目的代码基线，也不会提交到本仓库。

当前能力包括：

- 漫画宠物表情、心情与待机睡眠状态机。
- 触摸唤醒、语音对话、ASR、TTS 和流式 PCM 播放。
- AI 表情、灯光、绘画、摄像头观察和小车动作编排。
- BLE 配网与控制、SoftAP 摄像头视频和横屏遥控器。
- 喂食、戳一戳、石头剪刀布、连点比赛和摇骰子等儿童交互。
- 设备登记、加密保存供应商凭据、OTA 和管理页面。

## 仓库结构

- `firmware/head/`：ESP32-S3 头部固件，目标为 16 MiB Flash、8 MiB PSRAM。
- `android/`：Android App，使用 Kotlin 与 Jetpack Compose。
- `src/`、`public/`、`test/`：Node.js 云端、管理页面和测试。
- `firmware/components/`：可复用 ESP-IDF 组件。
- `docs/`：架构、设备协议和硬件联调记录。
- `ml/wakeword/`：本地唤醒词调研与训练说明。
- `agent-readme.md`：硬件事实、边界、风险与长期开发记录。

本地工具链、上游参考仓库、设备 Flash 备份、构建结果、APK、实际 `sdkconfig` 和凭据均被 `.gitignore` 排除。

## 云端

要求 Node.js 24 或更高版本。

```bash
cp .env.example .env
openssl rand -base64 32
set -a
source .env
set +a
npm install
npm test
npm run check
npm start
```

首次启动必须提供至少 12 位的 `ADMIN_PASSWORD`，并把 `MASTER_KEY` 替换为独立生成的随机值。本地 HTTP 调试可设置 `COOKIE_SECURE=false`；公网部署必须使用 HTTPS。

## Android

```bash
cd android
./gradlew :app:assembleDebug
```

本地 Android SDK 路径保存在未提交的 `android/local.properties`。当前仓库尚无 Android 单元测试源码，`testDebugUnitTest` 显示 `NO-SOURCE` 不代表已有测试覆盖。

## 固件

使用 ESP-IDF 6.1：

```bash
idf.py -C firmware/head set-target esp32s3
idf.py -C firmware/head build
```

依赖版本由 `firmware/head/dependencies.lock` 固定，托管组件会在构建时恢复。设备身份、Wi-Fi、云厂商密钥和生成的 NVS 镜像不得提交到仓库。烧录前先确认硬件版本并备份原始 Flash。

更完整的硬件事实、限制和安全要求见 [agent-readme.md](agent-readme.md)，接口见 [docs/device-api.md](docs/device-api.md)，架构见 [docs/architecture.md](docs/architecture.md)。

## 安全说明

- 第三方长期密钥只保存在服务端，不下发到 App 或固件。
- 每台设备使用独立、可撤销的设备令牌。
- `.env`、NVS 凭据镜像、私钥、设备备份和构建产物不得进入 Git。
- 聊天或日志中暴露过的令牌应立即轮换，不能因为仓库扫描未发现就继续视为安全。

部署前还需完成 Secure Boot、Flash Encryption、NVS Encryption、正式 TLS 域名、密钥轮换和运动安全验收；当前状态是开发验证版本，不是量产安全基线。
