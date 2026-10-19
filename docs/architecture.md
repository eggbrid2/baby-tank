# Baby Tank Cloud 架构与边界

## 目标

Baby Tank Cloud 是单用户、自托管的设备控制面。它负责管理员登录、设备身份、固件发布、OTA 状态、AI/ASR/TTS 服务配置和供应商协议适配。它不把第三方 API Key 下发给设备。

## 组件

- 一个 Node.js 24 进程提供 JSON API 和内嵌管理页面。
- SQLite 保存用户、会话、设备、服务配置、发布记录和审计日志。
- 固件以普通文件保存在持久化数据目录，元数据和 SHA-256 保存在 SQLite。
- Nginx 终止 TLS 并把流量转发到仅监听 `127.0.0.1` 的应用进程。
- ESP32 仅保存一枚可吊销的设备令牌。第三方云密钥使用服务端主密钥 AES-256-GCM 加密。

## 信任边界

管理会话与设备身份分离。管理员使用 HttpOnly 会话 Cookie 与 CSRF Token；设备使用 Bearer Token，OTA 下载也支持 TLS 上的 HTTP Basic，以兼容 `esp_https_ota`。数据库泄露后，攻击者能获得设备令牌哈希，但无法直接还原令牌；第三方密钥还需要服务器的 `MASTER_KEY` 才能解密。

`MASTER_KEY` 丢失后，已保存的第三方密钥无法恢复；泄露后必须轮换所有第三方密钥。因此它只能放在系统服务的环境文件或秘密管理器，不能提交到仓库或数据库。

## OTA 状态机

```text
published -> available -> downloaded -> installed -> pending verify
                                                   |-> validated
                                                   `-> rollback / failed
```

服务端负责板型、芯片、安全版本、发布通道和灰度资格检查。设备负责最终安全判断：TLS 证书校验、镜像格式、芯片型号、签名、`secure_version`、非当前槽写入以及启动自检。任何一层的“通过”都不能替代其他层。启用 anti-rollback 时采用纯 `ota_0 + ota_1`，不保留 factory 分区；不可恢复故障通过本机 USB 受控重刷处理。

## AI 与语音适配

设备侧协议与供应商协议解耦：ESP32 只提交设备令牌、文本或音频；服务端解密当前启用的凭据并请求供应商。

- LLM：Qwen、OpenAI、DeepSeek 和自定义中转站统一走 OpenAI Chat Completions 协议；支持模型选择、供应商方言、思考开关、推理强度、温度、最大输出 Token、普通 JSON 与 SSE 流式响应。不同供应商的思考参数由服务端适配，设备不直接感知差异。
- ASR：已接阿里百炼 Paraformer 实时 WebSocket。设备使用一次 HTTP 音频上传，服务端按约 100 ms 分帧发送给上游，最终返回统一 JSON。
- TTS：已接火山 Seed‑TTS 2.0 HTTP SSE。服务端解码 SSE 内的 Base64 音频片段，并以原始 MP3、PCM 或 Ogg Opus 流返回设备。

后台仍保留其他 ASR/TTS 供应商的数据模型，但它们尚无实际协议适配器，选择后调用会返回 `501`。当前 HTTP ASR 是“一句话录完再上传”，不支持边录边传或打断；若产品需要更低首字延迟，下一阶段应增加设备到服务端的 WebSocket 全双工通道，而不是改变供应商密钥边界。
