# Android、BLE 配网与离线控制记录

日期：2026-10-16
固件源码版本：`baby_tank_head 0.12.0`
设备烧录：未执行；实物仍运行已验证的 `0.7.0`

## 结论

Android 原生 App 和头部固件已经接到同一套 Espressif Network
Provisioning 协议，构建均通过。BLE 不是只用于首次配网：加密会话内还注册了
`baby-control` 自定义端点，保留无路由器、无互联网时的基础控制通道。

摄像头视频不走 BLE。手机无法通过解码或 AI 计算弥补无线链路带宽；视频应走
家庭局域网、机器人 SoftAP 或手机热点。BLE 只承担方向、速度、急停、灯光和
低频遥测，使高负载视频不阻塞安全控制。

## 已实现

### 固件

- 固定 `espressif/network_provisioning 1.3.1`，启用 NimBLE 和 Protocomm
  Security 1。
- 设备名为 `BabyTank-XXXXXX`，后缀取 Wi-Fi STA MAC 末三字节；不暴露设备
  Token。
- 每台设备从 NVS 读取独立 `setup_pop`，缺失时不降级到明文配网。
- 配网凭据由官方组件验证并保存；事件日志不打印 SSID、密码或配对码。
- 兼容旧开发镜像中的 Wi-Fi 字段，并把它们迁移到 IDF Wi-Fi 存储。
- 配网完成后不自动关闭 BLE；`baby-control` 端点继续存在。
- 控制包固定 8 字节：版本、操作码、序号、标志、左/右履带千分比。
- Ping 返回成功；运动和灯光在底盘未接入时返回 `UNAVAILABLE`。
- TTS 增加 4,000 sample（250 ms）启动预缓冲，并记录 ASR、LLM 与 TTS
  首音频延迟，不记录识别文本或模型回答。

### Android

- Compose 项目位于 `android/`，最低 Android 8.0。
- 通过官方 `esp-idf-provisioning-android 2.4.4` 扫描、连接和配网。
- 配对页要求设备专属配对码，再允许发送家庭 Wi-Fi。
- 硬件实验室可向 `baby-control` 发送加密 Ping，只有收到兼容回执才显示通过。
- 遥控摇杆和急停仍禁用，直到固件上报真实底盘能力并完成失联停车测试。

## 离线玩法

只有“一台手机 + 一台 Baby Tank”即可：

1. 无 Wi-Fi 时，BLE 直接完成基础遥控。
2. 看视频时，机器人可创建临时 SoftAP，手机连接它；无需第三台设备。
3. 需要机器人直接访问云 AI 时，可让机器人加入手机热点。
4. 后续可增加手机代理模式：手机连机器人 SoftAP，同时用蜂窝网络调用云 AI，
   再把结果回传给机器人。

第 4 种模式会增加 Android 网络绑定、后台保活、音频转发和供应商鉴权复杂度，
本次没有假装它已经完成。

## 构建验证

- `idf.py -C firmware/head build`：通过。
- 固件镜像：`0x1ff1c0` 字节；最小应用分区 `0x600000`；剩余 67%。
- 固件 SHA-256：`3ba4879a2ebb734048f9141f5490cc443f9bc1d179c25ea9c5a734dc983fa9b3`。
- 纯 C 控制包解析/编码主机单元测试（`-Wall -Wextra -Werror`）：通过。
- `./gradlew :app:assembleDebug`：通过。
- APK SHA-256：`596aa64224def3f96270c49b8a1a1a206eda2c21f0aa8562d697d2069d2b7055`。
- 使用虚构 Token/配对码、无 Wi-Fi 字段生成 16 KiB NVS 镜像：通过，权限
  `0600`。
- 未烧录、未连接真实 BLE 外设、未驱动电机。

## 风险与下一步

- Security 1 比明文安全，但量产更适合评估 Security 2、NVS Encryption、
  Secure Boot 和 Flash Encryption。
- 当前官方 Android 库依赖较重，并在其调试源码中包含敏感参数日志语句；正式
  发行前应固定并审计/修补该依赖，禁止把配对码写入发布日志。
- 常驻 provisioning manager 是第一版复用加密通道的实现，长期产品应评估独立
  的 BLE 控制服务，避免配网资源常驻并缩小可攻击面。
- ESP32-S3 的 Wi-Fi/BLE 共用 2.4 GHz 射频。摄像头压力测试必须同时测量 BLE
  指令往返延迟、丢包和底盘停车时间。
- 接入底盘前先确认 ESP32-C2 固件与 GPIO38/GPIO48 UART 方向，底盘必须在
  500 ms 内独立检测失联并停车，不能依赖 Android 或头部任务调度。
