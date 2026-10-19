# Baby Tank 安全 OTA 基线

此目录定义自研固件的 OTA 安全边界，不沿用上游示例的单 `factory` 分区。

## 16 MB 分区

`partitions_16mb.csv` 预留两个 3.5 MB OTA 槽、约 8.56 MB 资源区、加密设置区和 coredump 区。该布局只适用于已经确认是 16 MB Flash 的硬件；N8 模组不能使用。正式签名镜像大于 3.5 MB 时不能硬塞，必须缩减镜像或重新分配资源区。

分区表有意不含 `factory`。ESP-IDF 的 anti-rollback 配置要求 `ota_0 + ota_1` 且不带 factory 分区；保留一个低安全版本的恢复镜像也会与防降级目标冲突。最终救援路径是本机 USB 受控重刷，而不是一个永远可启动的旧镜像。

## 首次启动确认

新镜像启动后保持 `PENDING_VERIFY`，完成以下自检后才能调用 `baby_tank_ota_confirm_current()`：

1. NVS 与关键配置可读。
2. PSRAM、显示、音频编解码器初始化成功。
3. Wi-Fi 在限定时间内可进入已知状态；不能把“家庭网络暂时断网”简单判为固件坏。
4. 主任务至少运行一个看门狗周期且没有崩溃。
5. 与底盘 UART 协议版本兼容，失联停车仍有效。

如果关键硬件或任务失败，调用 `baby_tank_ota_rollback_and_reboot()`。不要在 `app_main()` 一进入就确认，否则回滚机制形同虚设。

## Secure Boot 与 anti-rollback

- 开发阶段只启用 A/B 与软件回滚，`sdkconfig.ota-security.defaults` 明确关闭 anti-rollback，不烧录不可逆 eFuse。
- 发布候选镜像先在专用测试板验证签名、升级、断电恢复与回滚。
- 量产/家庭正式设备启用 Secure Boot V2 后，只发布相同信任根签名的镜像。
- `secure_version` 只在确认旧版本永远不再需要后递增。eFuse 位有限，不能把每个普通版本号都映射成一个安全版本。
- Flash Encryption、JTAG/UART 下载限制和密钥保管要作为独立的设备供应流程设计，不能仅靠 OTA 代码开启。

`sdkconfig.ota-security.defaults` 不自动启用 Secure Boot、anti-rollback 或 Flash Encryption，这是有意设计：误烧 eFuse 可能永久锁死开发板。`sdkconfig.production-security.example` 只是审阅清单，不能直接作为默认配置。最终生产配置必须由受控烧录流程生成。
