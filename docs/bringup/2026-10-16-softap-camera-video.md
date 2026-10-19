# 机器人 SoftAP 与摄像头 Wi-Fi 视频记录

日期：2026-10-16
固件源码版本：`baby_tank_head 0.13.0`
Android App 版本：`0.1.0`
设备烧录：未执行；实物仍运行已验证的 `0.7.0`

## 结论

机器人 SoftAP、OV2640 JPEG 采集、单客户端 MJPEG 服务和 Android 本地视频
预览已经在源码中接通，固件与 App 均能构建。它解决的是“一台手机 + 一台
机器人、没有路由器时如何看视频”：控制继续走 BLE，视频走机器人自己的
Wi-Fi 热点，不需要第三台设备。

这里不能把“构建通过”写成“实物可用”。摄像头引脚、电源稳定性、真实帧率、
热量、BLE 延迟和 AP+STA 共存都还没有在当前设备上测量。遵照用户要求，本次
没有烧录。

## 传输设计

| 链路 | 第一版职责 | 原因 |
| --- | --- | --- |
| BLE | 配网、Ping、未来的运动/急停/低频遥测 | 低带宽控制不能被视频阻塞 |
| 机器人 SoftAP | 摄像头 MJPEG | 一部手机即可直连，不依赖路由器或互联网 |
| 家庭 Wi-Fi STA | 机器人访问云端 ASR/LLM/TTS | 与本地视频分离 |
| 手机蜂窝网络 | App 自身的互联网请求 | Android 只把视频 socket 绑定到机器人网络 |

ESP32-S3 以 AP+STA 模式运行。SoftAP 和 BLE 名称统一为
`BabyTank-XXXXXX`，后缀来自 Wi-Fi STA MAC 末三字节。SoftAP 只允许一台
客户端，使用 WPA2；密码不是明文配对码，而是：

```text
lowerhex(first_8_bytes(SHA-256("baby-tank-softap-v1:" + setup_pop)))
```

结果固定为 16 个小写十六进制字符。固件不打印密码，Android 不保存输入的
配对码。配对码仍应是每台设备唯一、随机且至少 16 位；派生不会增加原始配对码
的熵，弱配对码仍可能被离线猜测。

## 固件实现

- 使用官方 `espressif/esp32-camera 2.1.8`。最初评估的旧版 `2.0.5` 与本项目
  ESP-IDF 6.1 依赖模型不兼容，因此没有强行绕过组件声明。
- 按已核验的板级映射初始化 OV2640：XCLK 15 MHz，原生 JPEG，
  `FRAMESIZE_240X240`，质量 16，两个 PSRAM 帧缓冲，
  `CAMERA_GRAB_LATEST`。
- 限制为约 8 FPS、一个视频客户端、单帧同步发送。摄像头缓冲区直到发送完成
  才归还，避免上游异步发送后立即归还 framebuffer 的生命周期竞态。
- `GET http://192.168.4.1/health`：相机与堆内存状态。
- `GET http://192.168.4.1/snapshot.jpg`：单帧 JPEG。
- `GET http://192.168.4.1/stream`：MJPEG 流。
- HTTP 服务本身监听系统网络栈，但每个处理器都会检查 socket 的本地接口；
  只有目的地址为 SoftAP 的 `192.168.4.1` 才放行。家庭 Wi-Fi STA 地址访问会
  返回 `403`。
- 本地配对码与云端设备令牌分开读取；云令牌缺失或失效只关闭云对话，不会
  连带关闭 BLE 配网、SoftAP 或本地摄像头。

官方组件说明建议 Wi-Fi 场景使用 JPEG，并指出除低分辨率 JPEG 外通常需要
PSRAM；连续模式的两个以上帧缓冲只应配合 JPEG 使用：
[Espressif esp32-camera 2.1.7 README](https://components.espressif.com/components/espressif/esp32-camera/versions/2.1.7/readme?language=en)。
本项目实际锁定 `2.1.8`。

## Android 实现

- Android 10 及以上通过 `WifiNetworkSpecifier` 请求连接指定
  `BabyTank-XXXXXX`，由系统显示确认对话框。
- Android 13 及以上申请 `NEARBY_WIFI_DEVICES`；旧版本使用位置权限。
- 视频连接使用 `Network.openConnection()`，只把
  `http://192.168.4.1/stream` 放到机器人网络，不调用全进程网络绑定。
- App 解析 JPEG SOI/EOI，单帧上限 256 KiB，显示实际解码帧率，并在断流后
  最多重试三次。
- Android 8/9 不能使用 `WifiNetworkSpecifier`，需要先在系统 Wi-Fi 设置中
  手动连接热点。
- App 全局禁止明文 HTTP，只为固定本地地址 `192.168.4.1` 例外。

## 构建验证

- `idf.py -C firmware/head build`：通过。
- 固件镜像：`0x21c3e0` 字节（2,212,832 字节）；最小应用分区
  `0x600000`；剩余 65%。
- 固件 SHA-256：
  `24840e73f1ebc6e9a367d2da7ff3387e2bd9248a32b0625e74b1900d3cbb2bd1`。
- 纯 C 控制协议主机测试（`-Wall -Wextra -Werror`）：通过。
- `./gradlew :app:assembleDebug :app:lintDebug :app:testDebugUnitTest`：
  通过；Lint 为 0 error/4 个依赖版本提示，Android 项目当前没有 JVM 单元测试
  用例，不能把 `NO-SOURCE` 误报成测试覆盖。
- Debug APK SHA-256：
  `f0037db579695b0029211bee462243a06ba98fb870acfc7bcd2e6d8d7e8097ce`。
- 没有烧录、没有真实连接 OV2640、没有在 Android 真机接受网络确认框。

## 必须实测的风险

1. 该硬件已有“低电量下同时启用摄像头和 Wi-Fi 可能重启”的记录。首次测试应
   用稳定 USB 供电，同时记录复位原因和最低电压，再测试电池低电量。
2. ESP32-S3 的 Wi-Fi 和 BLE 共用 2.4 GHz 射频。视频压力下必须测 BLE Ping
   延迟、丢包，以及未来底盘的 500 ms 独立失联停车，不能只看画面流畅度。
3. AP+STA 共用一个 Wi-Fi 射频和信道；STA 接入家庭路由器时，SoftAP 信道会受
   STA 约束，切网或漫游可能让手机视频短暂断开。
4. MJPEG 240x240/约 8 FPS 是低延迟预览，不是“高清视频”。更高分辨率或帧率
   会增加 PSRAM、CPU、射频和耗电压力，不能在实测前承诺。
5. SoftAP 当前开机常驻，便于第一轮联调但增加耗电和暴露面。量产版应改为由
   BLE 或本机交互按需开启并自动超时关闭。
6. WPA2 密码依赖配对码熵，且 BLE Security 1 与 SoftAP 共用同一份根秘密。
   量产前应评估更强的设备证明、NVS Encryption、Secure Boot 和 Flash
   Encryption。

## 实物验收顺序

1. 轮换此前在对话中暴露过的设备令牌，并生成唯一高熵配对码。
2. 备份并校验原始 Flash 后，按单独批准的烧录步骤写入固件与 NVS；本记录不
   代表烧录授权。
3. 只开 SoftAP 和摄像头，检查 `/health`、单帧、连续 10 分钟视频及断线重连。
4. 加入 BLE Ping，统计视频开/关时的延迟和丢包差异。
5. 再加入家庭 Wi-Fi 云语音，验证 AP+STA、视频、ASR/TTS 和屏幕并发。
6. 最后接底盘，并以底盘独立失联停车为验收门槛。
