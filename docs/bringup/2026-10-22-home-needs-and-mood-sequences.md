# 首页需求状态、可唤醒睡眠与心情片段

日期：2026-10-22

## 产品判断

- 首页不再把 `mood / band / needs` 拼成一排调试式文字。角色脸负责表达情绪，首页只保留饭碗、水滴、睡眠三个可行动需求。
- 三项需求使用五格视觉量表；只有达到阈值时才显示“饿了 / 渴了 / 困了”。无新快照时显示空格，不把“待同步”伪装成宠物状态。
- 困倦时出现唯一主操作“去睡觉”。它不是本地 UI 假状态：App 先断视频、STOP、退出遥控，再经 BLE 要求头部结束前台互动并进入普通睡眠。普通睡眠保留 WakeNet 和头顶唤醒，和深夜锁分离。
- 休息成功后以幂等家长事件更新云端精力和体力；即使云端同步失败，头部睡眠也不回滚。

## 状态分布

- `sleepy / worried / annoyed / sad` 等需求状态优先于正向心情，避免高 joy 把饥饿、口渴和疲劳遮住。
- `happy` 与 `excited` 提高触发门槛；`curious` 有独立区间。
- 每个长期心情映射到 12 个表现片段。一个片段由 2–3 个现有基础表情组成，并变化强度和时长；相邻片段去重。没有新增位图资源。

## 协议与发布

- BLE `baby-diagnostics` 新增 opcode `12`：普通可唤醒睡眠。
- Android：`0.3.4 (22)`，APK：`android/app/build/outputs/apk/debug/app-debug.apk`。
- 云端发布前备份：`/opt/baby-tank-cloud/backups/2026-10-22-home-needs-mood/`。
- 头部回滚镜像：`/tmp/babytank-home-needs.DpFeB0/ota_0_before.bin`。
- 头部只刷应用分区，Wi-Fi 与其他 NVS 保留。

## 验证

- 云端：31 项 Node 测试通过，包括需求优先级和休息事件。
- Android：`assembleDebug` 与 `testDebugUnitTest` 通过。
- 固件：ESP-IDF 6.1 编译、应用分区刷写和哈希校验通过；镜像 `0x434400`，6 MiB 分区剩余约 30%。
- 实机串口确认一个环境表现片段连续播放 `shy → happy → proud`，随后恢复长期心情；没有额外图片内存。
