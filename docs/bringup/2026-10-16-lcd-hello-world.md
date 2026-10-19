# LCD Hello World 点亮记录

> 状态更新：本文记录的 `0.2.0` 已于同日升级为 `0.3.0` 自研猫脸版本。当前状态见 `2026-10-16-pet-face.md`。

日期：2026-10-16
固件：`baby_tank_head 0.2.0`
目标：使用纯自研代码点亮 ESP-SparkBot 头部屏幕并显示 `HELLO WORLD`。

## 实现边界

- 使用 ESP-IDF v6.1 原生 `esp_lcd` 和 ST7789 驱动。
- 不使用小智源码、云服务或协议。
- 不引入 LVGL；本次使用自研最小 5×7 字模直接生成 RGB565 帧。
- 摄像头、音频、触摸和底盘 UART 仍保持未初始化。

## 已确认屏幕参数

| 参数 | 值 |
| --- | --- |
| 控制器 | ST7789 |
| 分辨率 | 240×240 |
| 像素格式 | RGB565，16 bit |
| SPI Host | SPI3 |
| MOSI | GPIO47 |
| CLK | GPIO21 |
| CS | GPIO44 |
| DC | GPIO43 |
| Reset | 无独立 GPIO |
| 背光 | GPIO46，高电平开启 |
| SPI 模式 | Mode 0 |
| 像素时钟 | 40 MHz |

GPIO46 后续还可能与音频功放使能存在硬件复用冲突。当前音频未初始化，所以点亮屏幕没有竞争；加入音频功能前必须单独验证这一复用关系，不能让显示与功放驱动同时无协调地改写该引脚。

## 绘制方式

应用在内部 DMA 内存中分配 240×240×2 字节帧缓冲，清零形成黑色背景，再使用 5×7 字模、3 倍缩放在屏幕中央绘制白色 `HELLO WORLD`。帧缓冲保持常驻，避免异步 SPI 传输期间被释放。

相关源码：

- `firmware/head/main/display.c`
- `firmware/head/main/display.h`
- `firmware/head/main/app_main.c`

## 构建与烧录结果

- 构建成功，无项目源码编译错误。
- 应用版本：`0.2.0`
- ESP-IDF：`v6.1`
- ELF SHA-256：`6c466d9c0b106a4cd9e153495b5b05ae092318e32e8a3561afe95ba9ee34ad0a`
- 应用镜像 SHA-256：`fdd5b25668652abb7ac57ad02d6f17233399b02fcf27c493f4b56af4edded70e`
- 串口差分烧录完成，写入数据哈希验证通过。

## 启动验证

真实硬件串口日志确认：

```text
project=baby_tank_head version=0.2.0
baby_tank_display: ST7789 initialized; rendered HELLO WORLD at 240x240
baby_tank: display bring-up passed
baby_tank: heartbeat=0
baby_tank: heartbeat=1
baby_tank: heartbeat=2
```

未观察到初始化错误、崩溃或重启循环。

## 尚需人工确认

USB 串口只能证明控制器初始化、帧提交和程序持续运行，不能观测实体 LCD 的发光与像素内容。需要现场肉眼确认文字是否可见、方向是否正确以及黑白是否反相。如果物理显示异常，优先调整 ST7789 的颜色反相、SPI mode、镜像方向或背光复用策略，而不是把“日志成功”等同于“画面一定正确”。
