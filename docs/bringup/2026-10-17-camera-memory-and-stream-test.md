# 2026-10-17 摄像头与内存实测

## 硬件与不能混淆的容量

- 头部 ESP32-S3：512 KiB 片内 SRAM（芯片规格），实物检测到 8 MiB PSRAM。
- 当前镜像刚进入应用时，ESP-IDF 报告可管理的片内 heap 总量 290,267 B，PSRAM heap 总量 8,373,644 B。它们不是芯片物理总量；指令、静态段、缓存和保留区不能再次计入动态 heap。
- 链接映射中的 DIRAM 静态使用 204,586 B（代码 121,075、`.data` 43,143、`.bss` 40,368）；另有 IRAM 16,384 B、PSRAM `.bss` 14,964 B。链接映射与运行时 heap 统计口径不同，不能直接相加称为“模块占用”。

## 固件 0.13.12 的启动阶段净占用

单位均为字节。阶段差值包含驱动、任务、协议栈及系统动态分配，**不是**精确的逐函数分配追踪。

| 阶段 | 片内空闲 | 相比上阶段减少 | PSRAM 空闲 | 相比上阶段减少 |
| --- | ---: | ---: | ---: | ---: |
| 应用早期 | 237,287 | — | 8,369,816 | — |
| 屏幕／表情启动后、网络前 | 230,495 | 6,792 | 8,254,612 | 115,204 |
| Wi‑Fi／蓝牙／网络后 | 109,199 | 121,296 | 8,144,884 | 109,728 |
| 音频驱动后 | 93,215 | 15,984 | 8,117,112 | 27,772 |
| AFE／WakeNet／录音后 | 48,991 | 44,224 | 7,343,896 | 773,216 |
| GC2145／JPEG／HTTP 后 | 14,499 | 34,492 | 7,099,300 | 244,596 |

已知的大块分配：屏幕 240×240 RGB565 帧缓冲约 115,200 B PSRAM；摄像头 240×176 RGB565 帧缓冲 84,480 B PSRAM；优化 JPEG 输出缓冲 131,072 B PSRAM；摄像头内部 DMA 暂存 7,680 B。语音录音缓冲和 AFE 模型优先在 PSRAM，实时音频采样缓冲仍在片内。`esp_new_jpeg` 自身以及 HTTP／摄像头任务也使用片内 RAM，单靠 PSRAM 空闲不能保证可运行。

最初 0.13.11 在视频＋语音并发时，片内 heap 最低记录到 **200 B**、最大连续块仅约 1 KiB，视频重连且云端对话失败。0.13.12 将 Wi‑Fi／LwIP 可迁移缓冲优先放到 PSRAM，并将 NimBLE 动态内存放到 PSRAM；视频与语音并发时片内 heap 历史最低 **13,948 B**，视频未再因内存中断，WakeNet、云端问答和 BLE 重连均已实测。仅比较最小空闲值不能归因每笔分配；精确归属需要另行启用 ESP-IDF heap tracing。

## 成像与帧率对照

- 实物摄像头探测为 GC2145，原生输出 RGB565、无原生 JPEG。以 PSRAM DMA 直接接收 240×176 RGB565 时，原始帧已有雪花／错行；手机只是显示了坏数据。
- 改用片内 DMA 暂存后复制到单个 PSRAM 帧缓冲，240×176 原始帧和 JPEG 画面均未见同样错行。未采用上游双帧缓冲：Espressif 明确建议双缓冲连续模式只在原生 JPEG 输出时使用，RGB/YUV 写 PSRAM 在 Wi‑Fi 并行下可能丢数据。
- 240×176 RGB565 原样经 Wi‑Fi 传手机，每帧 84,480 B；本机实测发送约 0.4–1.1 s，偶发更久，约 1–2 fps。手机解码不是主要瓶颈。
- RGB565→`esp_new_jpeg` 1.0.2→MJPEG 后，典型 JPEG 约 4–7 KiB、编码约 27–44 ms、抓帧约 70–110 ms、发送通常几到几十 ms；Android 实际接收且原始 JPEG 抽检正常。0.13.12 长流的头部日志约 6–7 fps，比 RGB565 直传快，但仍不是高帧率视频。
- 视频开启时 TTS 供音最大间隙约 1.15–1.37 s；关视频后仍出现约 818 ms 间隙。因此视频可能加重 TTS 卡顿，不能认定它是唯一根因。

## 尚未解决或不应过度推断

- 偶发 `cam_hal: EV-VSYNC-OVF`、帧耗时抖动；还需长时测试与镜头实际清晰度／对焦检查。
- 不应把音频 DMA、摄像头 DMA 或需要片内访问的任务栈强行迁到 PSRAM。ESP-IDF 的分能力 heap、最大连续块和低水位应同时观察。
- 关视频后的语音仍有 TTS 供音间隙，需要另行追踪云端输出、网络接收和设备播放缓冲；本轮不把它误写成“摄像头已修复 TTS”。

## 参考

- [ESP32-S3 数据手册](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf)
- [Espressif esp32-camera 说明：RGB／PSRAM／Wi‑Fi 与帧缓冲约束](https://github.com/espressif/esp32-camera)
- [Espressif esp-vision RGB565→Wi‑Fi MJPEG 示例](https://github.com/espressif/esp-vision/blob/master/example/01-Camera/03-MJPEG/wifi_mjpeg_stream.py)
- [ESP_NEW_JPEG v1.0.2 更新记录：支持大端 RGB565](https://components.espressif.com/components/espressif/esp_new_jpeg/versions/1.0.2/changelog?language=en)
- [ESP-IDF heap 统计与连续块含义](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/mem_alloc.html)
