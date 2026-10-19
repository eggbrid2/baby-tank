# Baby Tank 本地唤醒方案记录

日期：2026-10-16
目标固件：`baby_tank_head 0.10.0`
状态：架构和上游源码已核验；错误的 ASR 文本唤醒入口已移除；产品唤醒词已改为纯中文“喵喵同学”；ESP-SR 2.6.0 与 `wn9_miaomiaotongxue_tts` 已生成模型分区镜像并通过构建；自训练已暂停；尚未接入音频 Codec、AFE 运行任务或真机模型测试；按要求未烧录。

## 结论

可以实现与原小智相同类型的本地唤醒，但不能把云 ASR 当成唤醒器。

待机时的正确链路是：

```text
麦克风 -> I2S 16 kHz/16-bit PCM -> ESP-SR AFE -> WakeNet -> 本地唤醒事件
                                                        |
                                                        +-> 命中后才录制一句话
                                                               -> VAD 截止
                                                               -> 云 ASR
                                                               -> LLM
                                                               -> TTS
```

因此，待机阶段没有语音上传、没有 ASR 调用次数，也没有云端唤醒延迟。自由对话仍需要 ASR，但它只在本地唤醒后处理一段有上限的用户语音。

## 已核验事实

1. 上游 `factory_demo_v1/main/app/app_audio_record.c` 使用 `esp_srmodel_init("model")` 加载模型，在 AFE `fetch()` 返回 `WAKENET_DETECTED` 后才进入录音/VAD；对话结束后重新启用 WakeNet。
2. 上游 `volc_rtc_sparkbot` 默认模型是 `wn9_hilexin`，对应“Hi 乐鑫”。这证明原方案是设备端 WakeNet，不是持续调用云 ASR。
3. ESP-SR 官方文档把 WakeNet定义为面向低资源 MCU 的本地唤醒引擎；ESP32-S3 支持 WakeNet9/WakeNet10，一个模型可支持最多五个唤醒词。
4. 官方当前捆绑模型列表包含“喵喵同学”，模型名为 `wn9_miaomiaotongxue_tts`，可在 ESP32-S3 上使用。模型名称不是配置文本；固件仍需把该模型打包进 `model` 分区并通过 AFE 加载。
5. 官方提供两种定制路径：基于 TTS 样本的社区模型请求，以及商业离线定制。商业语料路线要求至少 20,000 条合格语料，并注明通常需要两到三周且收费。
6. 当前自研分区表已经预留 `model` 分区：偏移 `0x10000`、大小 `0xf0000`。是否足够必须以最终模型打包产物为准，不能仅凭分区名称推断。
7. 上游 SparkBot 音频证据为 I2S BCLK GPIO39、MCLK GPIO45、LRCLK GPIO41、DOUT GPIO42、DIN GPIO40；Codec 示例使用 ES8311。该引脚表尚未在当前自研固件和实物上重新验证。

官方资料：

- [WakeNet 说明](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html)
- [ESP-SR 内置唤醒词列表](https://github.com/espressif/esp-sr/blob/master/wakeword_list.md)
- [官方唤醒词定制流程](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html)
- [TTS 唤醒词模型申请入口](https://github.com/espressif/esp-sr/issues/88)

## 已确定的产品唤醒词

产品唤醒词固定为纯中文“喵喵同学”，采用 ESP-SR 自带的 WakeNet9 模型 `wn9_miaomiaotongxue_tts`。首版不再训练“宝宝坦克”microWakeWord 模型。

推荐顺序：

1. 将 `wn9_miaomiaotongxue_tts` 打包进现有 `model` 分区，接通 ES8311、I2S、AFE 和本地唤醒事件。
2. 业务层只依赖本地唤醒事件，不依赖具体模型名，保留未来替换模型的能力。
3. 产品化前在真实家庭噪声、电视声、儿童声线、1–3 米距离和设备扬声器播放期间测试召回率与误唤醒率。
4. 只有现成模型无法达到验收门槛时，才恢复 microWakeWord 自训练或申请乐鑫定制。

不选择“常驻 MultiNet 假装 WakeNet”作为正式方案。MultiNet 可以在本地识别自定义命令词，但中英文混合词支持有限、资源高于专用 WakeNet，而且近期官方仓库仍有中文 MultiNet 长时间常驻识别崩溃报告。它可做实验，不应未经耐久测试进入儿童设备。

## 固件接入边界

计划中的本地唤醒组件只输出可信事件，不输出文本：

```c
typedef enum {
    BABY_TANK_WAKE_DETECTED,
    BABY_TANK_SPEECH_STARTED,
    BABY_TANK_SPEECH_ENDED,
    BABY_TANK_WAKE_TIMEOUT,
    BABY_TANK_WAKE_ERROR,
} baby_tank_wake_event_t;
```

事件处理规则：

- `WAKE_DETECTED`：切换表情到 `listening`，暂停再次唤醒；
- `SPEECH_STARTED`：切换到 `user_speaking`，写入有长度上限的 PSRAM 环形缓冲；
- `SPEECH_ENDED`：停止录音后才调用一次 ASR；
- TTS 播放期间保持半双工，避免扬声器回答再次唤醒自己；
- 成功、取消、超时或错误后清空本轮缓冲并重新启用 WakeNet；
- WakeNet 任务不得直接控制屏幕、网络或电机，只向状态机投递事件。

之前新增的“把 ASR 文本标准化后匹配唤醒词”代码已经从生产固件删除。文本匹配最多只能用于唤醒后的日志或测试，不能触发待机设备。

## 分阶段验收

### A. 音频与现成模型 bring-up

- 连续读取真实麦克风 16 kHz PCM，无溢出、看门狗或显示卡顿；
- “喵喵同学”本地命中后屏幕立即进入 listening；
- 网络断开时仍能本地唤醒；
- 待机抓包确认没有音频上传；
- 连续运行至少 8 小时，记录内部 RAM、PSRAM、CPU 和误唤醒次数。

### B. 产品唤醒词模型

- “喵喵同学”在成人/儿童、男女声线及 1–3 米距离分别测试；
- 安静环境召回率、噪声环境召回率和每小时误唤醒次数必须用实测数字记录；
- 电视节目、手机外放和机器人自身 TTS 纳入负样本；
- 模型二进制版本、来源、许可和 SHA-256 写入发布记录。

### C. 云对话

- 只有本地唤醒且 VAD 结束后才发起 ASR；
- 每轮录音设置硬上限，失败不自动无限重试；
- 当前云配置尚无可用 ASR，因此在服务端配置完成前，不能宣称完整语音闭环可用；
- TTS 当前返回 MP3/24 kHz，必须先解码为 PCM 再播放和驱动嘴部包络。

## 安全记录

设备凭据不会写入源码、文档或构建产物。先前在对话中提供过的设备令牌应视为已经暴露；正式家庭试用前必须在服务端轮换，再通过设备安全配置流程注入，而不是硬编码进固件。

## 本次构建验证

- ESP-IDF：`v6.1`；目标：`esp32s3`；
- ESP-SR 固定为 `2.6.0`，只选择 `CONFIG_SR_WN_WN9_MIAOMIAOTONGXUE_TTS=y`；
- 模型镜像：291,173 字节；SHA-256：`77932226d163d7e454df469df676b6d3778923382abbbf8a74d24ed77abb19da`；
- 应用镜像：261,312 字节；SHA-256：`399485dc1b9d54e62b3c62721caffa003939ba2de0c2fd87b64cf8fc7a722601`；
- ELF SHA-256：`10db4603f81f37e7476a4dcad301f55554101cd579235171572702b300c470e9`；
- 未执行 `flash`、`write-flash`、串口复位或下载模式操作；设备仍运行原有已烧录版本。
