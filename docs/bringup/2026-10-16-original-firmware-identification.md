# 原机固件与 OTA 槽识别记录

日期：2026-10-16
设备：USB 连接的 ESP32-S3（16 MiB Flash、8 MiB PSRAM）
分析方式：只读 Flash 备份、ESP 镜像描述符解析、二进制字符串比对、公开源码与发行包核验。

> 历史状态说明：本文记录的是擦除前的原机状态。2026-10-16 后续已按用户决定整片擦除目标设备并写入自研 `baby_tank_head 0.1.0`。当前设备状态见同目录的 `2026-10-16-independent-firmware-cutover.md`，不要再把本文的 `ota_0=xiaozhi` 当作当前状态。

## 结论

原机当前可启动应用不是 `upstream/esp_sparkbot/example` 下某个示例工程的直接产物。

它高度对应公开项目 [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) 的 `v1.3.0`、`esp-sparkbot` 板型构建：

- ESP 应用描述符：`project=xiaozhi`、`version=1.3.0`。
- 应用编译时间：`2025-02-24 20:45:38`。
- 使用的 ESP-IDF：`v5.3.2-dirty`。
- 镜像内含 `main/boards/esp-sparkbot/esp_sparkbot_board.cc`、`chassis.cc` 等源码路径。
- `v1.3.0` 官方源码标签提交为 `799302e1e6acad559db510ec2268f6560283e3c4`，其中项目名、版本、板型目录和分区布局均与设备镜像吻合。
- 官方 `v1.3.0_esp-sparkbot.zip` 确实存在，但它不是设备镜像的逐字节同一构建：官方发行包应用使用 `ESP-IDF v5.4-dirty`，编译于 `2025-02-24 22:42:40`，设备镜像则使用 `v5.3.2-dirty`，ELF 哈希也不同。

因此可作如下分级判断：

1. **项目与板型：高置信度确定**——`xiaozhi-esp32 v1.3.0 / esp-sparkbot`。
2. **源码血缘：高置信度对应官方 `v1.3.0` 标签或非常接近的提交**。
3. **是否拥有原厂构建的精确源码状态：不能确认**——当前有公开标签源码，可以重建和继续开发，但无法仅凭二进制证明它与原厂工作区、依赖锁定状态完全一致。

## 与 `upstream/esp_sparkbot/example` 的关系

本地示例目录包含的完整工程包括：

- `factory_demo_v1`
- `openai-realtime-demo`
- `volc_rtc_sparkbot`
- `tank/sparkbot_motion_control`
- `tank/c2_tracked_chassis`
- 其他显示、人脸识别和检测示例

这些工程的 CMake 项目名均不是 `xiaozhi`，目录内也没有小智应用的核心代码、Tenclass OTA 地址或小智的板级注册结构，因此不能把当前 OTA 固件归为这些示例中的一个。

两者并非毫无关系。小智 `v1.3.0` 的 `esp-sparkbot/chassis.cc` 明确引用：

`https://gitee.com/esp-friends/esp_sparkbot/tree/master/example/tank/c2_tracked_chassis`

同时沿用了本地示例中的底盘控制约定：UART1、TX=GPIO38、RX=GPIO48、115200 波特率，以及 `x...y...`、`w...`、`d...` 命令。因此准确表述是：**小智是独立的上层应用工程，使用了 ESP-SparkBot 的硬件定义和底盘示例协议，而不是由该示例目录直接编译出来。**

## 源码可获得性

公开标签源码采用 MIT License。本次核验曾使用被 `.gitignore` 忽略的临时克隆；在切换到纯自研固件后，该克隆及下载的发行包已从项目工作区移入 macOS 废纸篓，不再作为工作区内容或产品依赖。若将来确有审计需要，应按本文记录的标签和提交重新获取，而不是把它合入自研源码。

## 当前分区与启动状态

| 分区 | 偏移 | 大小 | 状态 |
| --- | ---: | ---: | --- |
| `nvs` | `0x9000` | 16 KiB | 有数据；未解析凭据 |
| `otadata` | `0xD000` | 8 KiB | 序号 1、状态 `VALID`，选择 `ota_0` |
| `phy_init` | `0xF000` | 4 KiB | 有数据 |
| `model` | `0x10000` | 960 KiB | 模型/资源数据 |
| `ota_0` | `0x100000` | 6 MiB | 当前唯一有效应用：`xiaozhi 1.3.0` |
| `ota_1` | `0x700000` | 6 MiB | 全部为 `0xFF`，已经为空 |

分区表中没有 `factory` 应用分区。擦除 `ota_0` 后不会有出厂应用兜底；擦除或错误改写 `otadata` 也会增加启动选择的不确定性。

## 关于“清空 OTA”的决策

- **不擦除 `ota_0`**：它是目前唯一已知可启动、可用于硬件回归的原机固件。
- **无需擦除 `ota_1`**：逐字节检查确认它已经全部为 `0xFF`，可作为后续新固件的空闲槽。
- **不单独擦除 `otadata`**：目前它正确指向 `ota_0`。未来切槽应通过 ESP-IDF OTA 状态 API 或经过验证的串口烧录流程完成，而不是手工猜写 OTA 元数据。
- **不删除本地 `firmware/components/baby_tank_ota`**：这是 Baby Tank 自研固件的 OTA 组件源码，与原机 Flash 中的 OTA 槽不是同一个概念。

安全的后续顺序应为：保留完整 Flash 备份和 `ota_0`，先让自研固件在 `ota_1` 或独立测试板上启动并通过显示、音频、Wi-Fi、底盘失联停车等自检，再切换启动槽；只有新固件稳定后，才考虑复用原 `ota_0`。

## 恢复依据

完整 16 MiB Flash 备份：

`/Users/kirito/StudioProjects/baby-tank/artifacts/device-backups/20261016T071101Z-esp32s3-44bd8d694968/flash-full-16MiB.bin`

SHA-256：

`853b91b4fdb21d0cadbc76f53049db92f9537612a75207757ec504cf8092561e`

备份已经过设备端 `verify-flash` 校验。备份目录为本地受限目录并被 Git 忽略，不应上传到公开仓库，因为 NVS 中可能含网络配置或设备凭据。

## 未解决项

- 尚未取得与设备 ELF 哈希 `169134e5c6ff2228e981bcf95401fc0e4c1e526091f89569bfdb59655ff92852` 完全匹配的构建产物或构建清单。
- 尚未通过运行时串口日志再次确认当前运行分区；依据有效 `otadata`、`ota_0` 镜像和空白 `ota_1`，当前启动自 `ota_0` 的证据已经充分。
- 在开发新固件前仍需固定 ESP-IDF 版本、组件依赖和 ESP-SparkBot 引脚/外设自检基线。
