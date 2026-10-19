# ESP32 设备接口

所有路径以 `/api/device/v1` 开头。除健康检查外都要求设备令牌：普通 JSON 请求使用 `Authorization: Bearer <token>`；`esp_https_ota` 下载也支持 `Authorization: Basic base64(device_id:token)`。Basic 认证只能通过 HTTPS 使用。

## 心跳

`POST /heartbeat`

```json
{"firmwareVersion":"1.0.0"}
```

响应包含服务器时间和 AI 配置修订号。建议正常工作时每 60 秒发送一次，并加入随机抖动；离线不应阻塞本地急停与失联停车。

## 运行配置

`GET /config`

```json
{
  "deviceId": "tank-001",
  "revision": "2026-10-16T10:00:00.000Z",
  "services": {
    "llm": {"provider":"qwen","model":"qwen-plus","proxyUrl":"https://example/api/device/v1/llm"},
    "asr": {"provider":"qwen","model":"paraformer-realtime-v2","proxyUrl":"https://example/api/device/v1/asr"},
    "tts": {"provider":"volcengine","model":"seed-tts-2.0","proxyUrl":"https://example/api/device/v1/tts"}
  }
}
```

响应不包含任何供应商凭据。设备持久化上一次已验证配置；服务器暂时不可达时继续使用本地安全功能。

## OTA 检查

`GET /ota/check?board=baby-tank-s3&chip=esp32s3&version=1.0.0&secureVersion=0&secureBoot=0&channel=stable`

无更新：

```json
{"updateAvailable":false}
```

有更新：

```json
{
  "updateAvailable": true,
  "version": "1.1.0",
  "size": 2310400,
  "sha256": "...",
  "secureVersion": 0,
  "mandatory": false,
  "url": "https://example/api/device/v1/firmware/12"
}
```

服务端按设备 ID 稳定分桶实施灰度，不会因设备重启而随机变组。`mandatory` 只影响产品交互策略，不能绕过电量、运动状态和完整性检查。设备在电机运动中、电池电压不足或音频关键会话中不应开始刷写。

## 固件下载

`GET /firmware/:id` 支持 `Range: bytes=start-end`，返回 `206`、`Content-Range`、`Accept-Ranges: bytes` 和 `X-Firmware-Sha256`。设备仍须以签名验证作为最终信任依据，HTTP 响应头中的哈希不是签名替代品。

## OTA 状态上报

`POST /ota/report`

```json
{"event":"validated","version":"1.1.0","detail":"self-test passed"}
```

允许的事件为 `downloaded`、`installed`、`validated`、`rollback`、`failed`。`detail` 不得包含 Wi-Fi 密码、设备令牌或第三方密钥。

## 大模型代理

`POST /llm` 接受 OpenAI 兼容的 `messages`，服务端固定使用管理员选中的模型、供应商密钥和生成参数。后台可配置思考开关、推理强度、温度与最大输出 Token；后台已设置的参数优先于设备传入的 `temperature` 和 `max_tokens`。设备不能覆盖模型、思考策略或上游地址。流式响应使用上游 SSE 原样转发。

```json
{"messages":[{"role":"user","content":"你好"}],"stream":true}
```

思考参数由供应商适配器转换：千问使用 `reasoning_effort`/`enable_thinking`，DeepSeek 使用 `thinking.type` 与 `reasoning_effort`，OpenAI 使用 `reasoning_effort`。明确开启思考时服务端不会发送温度参数，以避免供应商忽略或拒绝请求。自定义中转站需要在后台选择其兼容协议。

## 语音代理

### 阿里 ASR

`POST /asr?format=pcm&sampleRate=16000` 的请求体是单声道音频二进制，推荐 `Content-Type: audio/pcm`。支持 `pcm`、`wav`、`mp3`、`opus`、`speex`、`aac`、`amr`；默认最大 4 MiB，可由 `SPEECH_MAX_BYTES` 调整。PCM 必须是 16-bit little-endian 单声道。

```bash
curl -X POST 'https://example/api/device/v1/asr?format=pcm&sampleRate=16000' \
  -H 'Authorization: Bearer DEVICE_TOKEN' -H 'Content-Type: audio/pcm' \
  --data-binary @speech.pcm
```

成功响应：

```json
{
  "text": "你好，小坦克",
  "sentences": [{"text":"你好，小坦克","beginTime":0,"endTime":850}],
  "provider": "qwen",
  "model": "paraformer-realtime-v2"
}
```

这是“一句话录完再上传”的接口。服务端内部按约 100 ms 分帧调用 Paraformer，不等同于设备侧实时双工识别。

### 火山 TTS

`POST /tts` 接受 JSON 文本。服务端使用后台固定的音色、格式与采样率，设备不能覆盖这些计费和播放参数。

```bash
curl -X POST 'https://example/api/device/v1/tts' \
  -H 'Authorization: Bearer DEVICE_TOKEN' -H 'Content-Type: application/json' \
  -d '{"text":"你好，小坦克"}' --output reply.mp3
```

响应体是可直接保存或流式播放的原始音频，不是供应商 SSE；响应头给出 `Content-Type`、`X-Audio-Format` 和 `X-Audio-Sample-Rate`。默认文本上限 2000 字符，可由 `TTS_MAX_CHARACTERS` 调整。

### 后台配置要点

- 阿里 ASR：`provider=qwen`，API Key 必填，模型建议 `paraformer-realtime-v2`；Workspace ID 可选但建议填写。
- 火山 TTS：`provider=volcengine`，新版控制台填写 API Key；旧版鉴权可填写 App ID + Access Token。系统音色通常使用 Resource ID `seed-tts-2.0`；复刻/克隆音色必须使用该音色所属资源，例如当前 ICL 音色使用 `seed-icl-2.0`。Resource ID 与 Speaker ID 不匹配时，上游会拒绝请求。音色 ID 必填。
- 未实现的 ASR/TTS 供应商会返回 `501`；未配置、缺密钥或缺音色会返回 `503`。
- 服务端不记录音频内容；Nginx/应用访问日志也不应记录请求体。儿童语音的供应商侧留存策略仍需在正式上线前按账号设置复核。
