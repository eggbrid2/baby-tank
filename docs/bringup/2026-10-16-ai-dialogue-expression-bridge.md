# AI 对话与表情桥接记录

日期：2026-10-16
固件：`baby_tank_head 0.9.0`
状态：源码和本机构建验证完成；按用户要求未烧录、未进行音频真机测试。

## 本次完成内容

新增供应商无关的 `conversation.h/.c`，将对话系统事实映射为动画，不让模型直接控制屏幕几何或优先级。

| 对话阶段 | 默认表情 | 含义 |
| --- | --- | --- |
| `idle` | `calm` | 本轮结束，短暂回中性后允许环境动画恢复 |
| `listening` | `curious` | 已开启监听，等待用户说话 |
| `user_speaking` | `curious` | 本地或服务端 VAD 判断用户正在说话 |
| `recognizing` | `thinking` | 语音结束，等待最终识别文本 |
| `thinking` | `thinking` | 已有文本，模型正在生成回答 |
| `ai_speaking` | 模型语义情绪，缺省 `calm` | 正在播放回答音频 |
| `cancelled` | `calm` | 用户打断或本轮取消 |
| `error` | `worried` | 本轮可恢复错误 |
| `disconnected` | `sad` | 云连接不可用 |

除 `idle`、`cancelled` 和短错误提示外，活动对话状态使用可信固件的保持时长，直到对话状态机显式切换。模型入口仍被限制在 24 个情绪名称，不能设置优先级或无限时长。

## 说话口型

`baby_tank_conversation_observe_tts_pcm16()` 接收将要送入扬声器的 16-bit PCM 音频块：

- 每块最多 4096 个采样，函数不复制、不保存音频；
- 计算绝对值平均能量，扣除固定噪声底后映射到 `0..100`；
- 表情控制任务使用较快启动、较慢释放的包络平滑，避免逐帧抖动；
- 包络只改变现有奶油色嘴部的开合，不改变眼睛、颜色、角色比例或动画时间线；
- `sleepy`、`sleeping` 和 `bubble` 明确禁止说话口型，避免睡眠形象被破坏；
- 离开 `ai_speaking` 时立即把目标口型归零。

这是音量同步，不是音素级口型。对于当前只有简化嘴部的漫画角色，音量包络成本低且视觉稳定；如果以后要区分元音形状，需要 TTS 返回 viseme/phoneme 时间戳，不能只从 PCM 准确反推。

## 上游音频资料核验

上游仓库已经证明 SparkBot 音频硬件路径存在：

- `example/openai-realtime-demo/src/media.cpp` 的 SparkBot 分支声明 ES8311；
- I²C 为 SDA GPIO4、SCL GPIO5；
- I²S 为 BCLK GPIO39、LRCLK GPIO41、输出 GPIO42、输入 GPIO40；
- 示例使用 16 kHz 音频，ES8311 同时处理麦克风输入和扬声器输出；
- `factory_demo_v1/components/bsp_extra` 也包含独立的 ES8311/BSP 实现。

这些是源码证据，不等于当前自研固件已经完成真机音频验证。引脚、电平、时钟、声道、增益、V1.2 音频硬件修改和实际噪声底仍需在设备上逐项测试。

## 为什么没有直接合并旧 OpenAI demo

旧示例不能直接作为现在的对话实现：

1. 顶层 CMake 把 `OPENAI_API_KEY` 作为编译宏写进固件，长期密钥可被 Flash 读取；
2. 它调用旧的 `/v1/realtime?model=gpt-4o-mini-realtime-preview-2024-12-17` preview 接口；
3. 当前仓库中的 `components/peer/libpeer` 为空，示例本身缺少完整 WebRTC 子模块；
4. 断线或 HTTP 失败路径直接重启设备，不符合运动设备的故障隔离要求；
5. 它没有儿童设备所需的设备身份、撤销、配额、隐私和供应商治理。

当前 OpenAI 官方文档要求 GA Realtime WebRTC 使用 `/v1/realtime/calls`，并建议客户端使用由开发者服务端签发的短期 client secret，而不是将标准 API Key 放到客户端。官方也明确说明 beta 集成需要迁移事件形状和端点：

- [Realtime WebRTC](https://developers.openai.com/api/docs/guides/voice-webrtc)
- [Realtime API 入门与 GA 迁移](https://developers.openai.com/api/docs/guides/realtime)

因此，即使最终选择 OpenAI，也应由 Baby Tank 云网关保存长期 Key、签发或代理短期会话，设备只持有可撤销的设备凭据。若选择国内 ASR/LLM/TTS，设备侧的本次状态与表情接口无需变化。

## 未来接线点

正式音频/网络任务应按以下顺序调用：

```c
baby_tank_conversation_set_phase(BABY_TANK_CONVERSATION_LISTENING);
baby_tank_conversation_set_phase(BABY_TANK_CONVERSATION_USER_SPEAKING);
baby_tank_conversation_set_phase(BABY_TANK_CONVERSATION_RECOGNIZING);
baby_tank_conversation_set_phase(BABY_TANK_CONVERSATION_THINKING);

/* 可选：服务端返回经过白名单校验的回答情绪。 */
baby_tank_conversation_set_ai_emotion("happy", 72);
baby_tank_conversation_set_phase(BABY_TANK_CONVERSATION_AI_SPEAKING);

/* 每个即将写入 I2S 的 PCM 块先用于更新口型，再播放。 */
baby_tank_conversation_observe_tts_pcm16(pcm, sample_count);

baby_tank_conversation_set_phase(BABY_TANK_CONVERSATION_IDLE);
```

实际调用者还必须实现每阶段超时、取消、断线恢复和播放缓冲欠载处理，不能让保持状态永久挂住。

## 构建验证

- ESP-IDF：`v6.1`；目标：`esp32s3`；
- 构建命令：`idf.py -C firmware/head build`；
- 应用镜像：245904 字节（`0x3c090`）；
- BIN SHA-256：`f5d82b6f3c99230224bdd8385ac934e31b074067aa302f1b6f21979652f0929c`；
- ELF SHA-256：`7b21d1f11e05adc985e5dfb0786c4432bbe81018caf4e5250111158c061357c2`；
- 构建产物已确认包含 `0.9.0` 和 conversation adapter 日志；
- 没有执行 `flash`、`write-flash`、串口复位或下载模式操作；设备仍运行上一版 `0.7.0`。

## 尚缺的产品决定

“接入 AI”不是单一技术选项，仍必须确认：

- ASR、LLM、TTS 是一体化实时语音模型，还是三段式供应商；
- 中国大陆网络是否必须稳定直连；
- 是否使用已有公网服务器作为 Baby Tank 网关及其专用域名；
- 第一版触摸开始还是唤醒词开始；
- TTS 音色、儿童内容策略、会话保存和费用上限。

这些选择会改变云协议、延迟、成本和合规性，但不会改变本次完成的本地动画接口。
