# 生图转极简宠物脸记录

> 状态更新：本文记录的 `0.4.0` 静态位图已被 `0.6.0` 程序化线条动画替换。生成稿保留为历史资料，但不参与当前固件编译。当前状态见 `2026-10-16-procedural-line-face-animation.md`。

日期：2026-10-16
固件：`baby_tank_head 0.4.0`
目标：删除 `0.3.0` 猫头造型，只保留友好的眼睛和嘴巴，并从生成稿转换为可复现的嵌入式资产。

## 视觉决策

上一版把猫耳、圆脸、瞳孔、高光、腮红、鼻子和胡须同时放进 240×240 小屏，信息密度过高；大眼和高光还会强化“被凝视”的感觉。用户反馈“丑且诡异”是有效的设计信号，不应只靠微调配色修补。

新版本采用：

- 纯黑背景；
- 两只青色闭眼，避免直视感；
- 一个小型暖白微笑；
- 不包含头部轮廓、耳朵、鼻子、瞳孔、腮红、胡须、文字或装饰。

## 生成方式

使用 Codex 内置图像生成工具生成，不使用外部 GitHub 素材。最终提示词：

```text
Use case: stylized-concept
Asset type: face artwork for a 240x240 round embedded LCD on a small friendly pet robot
Primary request: create one minimal, lovable pet expression made from ONLY two eyes and one small mouth
Scene/backdrop: solid pure black square background, edge to edge
Subject: two softly rounded, slightly curved teal-cyan eyes with a gentle happy expression, and one tiny warm cream-colored smiling mouth centered below
Style/medium: ultra-clean flat 2D graphic, simple mascot UI, crisp hard-edged shapes, no gradients, no texture, no shadows, no glow, no antialias-dependent microdetails; designed to remain clear when reduced to 240x240 and RGB565
Composition/framing: centered and symmetrical; eyes occupy the middle horizontal band with comfortable spacing; mouth small and understated; generous black negative space around all features
Color palette: exactly black background plus teal-cyan eyes plus warm cream mouth
Constraints: ONLY eyes and mouth; friendly, calm, cute, non-human, non-creepy; no text; no watermark
Avoid: head outline, face circle, ears, nose, eyebrows, pupils, irises, cheeks, blush, whiskers, fur, teeth, tongue, body, icons, decorative marks, 3D rendering, photorealism, gradients, lighting effects, glow, shadow, background objects
```

原始生成稿：`firmware/head/assets/pet-face-source-v1.png`
原图 SHA-256：`7118b588181a6867c43174eef2f2734c627c56014f57545c921524f94ad48567`

## 转换流程

`tools/convert_pet_face.py` 执行以下确定性步骤：

1. 使用 Lanczos 缩放到 240×240。
2. 去掉模型残留的微弱渐变和发光，将画面量化为黑、青、暖白三色。
3. 转换为 RGB565。
4. 按扫描线顺序做 RLE 编码并生成 `firmware/head/main/pet_face_asset.h`。

输出：

- 屏幕预览：`firmware/head/assets/pet-face-240-v1.png`
- 预览 SHA-256：`4ef1cb297059dcdf8e2e5dfcb625c0887a2367ca28285365d772165a56e88e85`
- 像素数：57600
- 颜色数：3
- RLE 色段数：232
- 原始 RGB565 大小：115200 字节
- RLE 数据大小：928 字节

转换命令：

```sh
python3 tools/convert_pet_face.py \
  firmware/head/assets/pet-face-source-v1.png \
  firmware/head/assets/pet-face-240-v1.png \
  firmware/head/main/pet_face_asset.h
```

## 固件结果

- 版本：`0.4.0`
- 应用大小：234560 字节（`0x39440`），最小应用分区剩余约 96%
- ELF SHA-256：`888dcb2167708eeb3f748d705d4ebd95602e23e39f01cdb708b37d4bb837558f`
- 应用镜像 SHA-256：`3a5697435ce5df9350c8cbaef89da992f26031d66cc4ed8e1423750a21679f6c`
- `/dev/cu.usbmodem2121301` 差分烧录成功，写入后哈希校验通过。

真机串口日志：

```text
project=baby_tank_head version=0.4.0
baby_tank_display: ST7789 initialized; rendered generated minimal pet face at 240x240
baby_tank: display bring-up passed
baby_tank: heartbeat=0
baby_tank: heartbeat=1
baby_tank: heartbeat=2
```

未观察到初始化错误、崩溃或重启循环。串口无法验证实体屏幕的最终颜色和朝向，仍需人眼确认。

## 后续边界

本版是静态表情。若后续增加眨眼或说话动画，应把眼睛和嘴拆成少量独立帧或参数化遮罩，并只更新变化区域；不要为每一帧保存完整 240×240 图片，也不要引入庞大的 GUI 框架来解决三个简单图形的动画问题。
