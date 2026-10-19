# 自研固件切换记录

> 状态更新：本文完成时的 `0.1.0` 最小基线已于同日升级为 `0.2.0` 屏幕点亮版本。当前状态见 `2026-10-16-lcd-hello-world.md`。

日期：2026-10-16
目标设备：ESP32-S3 QFN56 rev 0.2，16 MiB Flash，8 MiB Octal PSRAM
USB 端口：`/dev/cu.usbmodem2121301`

## 决策

产品固件不使用小智应用、小智云服务或小智通信协议，只保留 Baby Tank 自研代码。

公开小智源码只曾用于识别原机固件和提取硬件事实，不合入正式固件。原机完整 Flash 镜像保留在被 Git 忽略的受限备份目录，用于故障恢复和证据追溯，不作为产品源码或运行时依赖。

## 擦除前保护

整片擦除前再次校验了原机完整备份：

- 文件：`artifacts/device-backups/20261016T071101Z-esp32s3-44bd8d694968/flash-full-16MiB.bin`
- SHA-256：`853b91b4fdb21d0cadbc76f53049db92f9537612a75207757ec504cf8092561e`
- `sha256sum -c SHA256SUMS`：通过
- 先前设备端 `verify-flash`：通过

备份可能含原机 NVS 凭据，不得提交或上传公开仓库。

## 自研镜像

- 项目：`baby_tank_head`
- 版本：`0.1.0`
- ESP-IDF：`v6.1`
- 应用大小：176704 字节（`0x2B240`）
- ELF SHA-256：`6aea4d08953fc07a3390c582bb1dfc4c155dc3d4aa591f8fa3939e177298df55`
- 应用镜像 SHA-256：`4ad5e2a50c29a65bc905c75c20baf4825fc337aa20da0430e117396155e1fb70`
- 镜像 validation hash：`93f7bf2c1a16951a00a1e4d034941f29aa3e42948e6cd2f9f79637c7d4b9f8d0`

烧录前对应用镜像执行字符串检查，未发现 `xiaozhi`、`tenclass` 或 `api.tenclass` 标记。

本次为适配 ESP-IDF 6.1，将主组件依赖从旧名称 `esp_flash` 改为 `spi_flash`，并显式加入提供应用描述符头文件的 `esp_app_format`。

## 擦除与烧录结果

已执行目标板整片 Flash 擦除，esptool 报告：

`Flash memory erased successfully`

随后写入并逐段校验：

| 偏移 | 内容 | SHA-256 |
| ---: | --- | --- |
| `0x000000` | 自研 Bootloader | `a3a5d75ee2e090949a97cc7ddf7e8c81f186cb2df56ba27622f49eb8fd72952c` |
| `0x008000` | 自研分区表 | `4d122ca60c7321c2c4cb393d3b612908263c2c860e92edd43036edbfd1c762e0` |
| `0x00D000` | 初始 OTA 元数据 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `0x100000` | `baby_tank_head 0.1.0` | `4ad5e2a50c29a65bc905c75c20baf4825fc337aa20da0430e117396155e1fb70` |

esptool 对所有实际写入段均报告 `Hash of data verified`。

因为执行的是整片擦除而非覆盖写入，原小智应用、NVS、模型数据和其他残留均已从设备 Flash 移除。当前 `ota_1` 仍为空，可用于后续 A/B 升级。

## 真实硬件启动验证

USB 串口日志确认：

- Bootloader：ESP-IDF `v6.1`
- 从 `ota_0`（`0x100000`）加载应用
- Project：`baby_tank_head`
- App version：`0.1.0`
- 芯片：ESP32-S3，双核，revision 0.2
- Flash：16777216 字节（16 MiB）
- PSRAM：8388608 字节（8 MiB），启动内存测试通过
- 应用连续输出 `heartbeat=0`、`heartbeat=1`、`heartbeat=2`
- 未观察到重启循环、崩溃或校验错误

## 当前能力边界

此版本是独立 bring-up 基线，不是完整产品固件。当前只初始化系统、USB 日志、Flash/PSRAM 检查与心跳；显示、触摸、音频、摄像头、Wi-Fi 和底盘 UART 尚未启用。因此设备不再显示或执行原小智功能是预期结果，不是启动失败。

后续开发应从本自研基线逐项启用外设，每项保留启动自检、失败降级和底盘失联停车保护。
