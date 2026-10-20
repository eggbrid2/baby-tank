# Baby Tank Head Firmware

This is the independent ESP32-S3 firmware for the Baby Tank head. It is not
based on an upstream ESP-SparkBot example.

The `0.14.29` build extends the safe bring-up baseline with a procedural animated
robot-baby face and a half-duplex voice conversation runtime:

- boot through the native USB Serial/JTAG interface;
- report chip, Flash, PSRAM, heap, reset reason, and build identity;
- initialize the 240x240 ST7789 over SPI3 and render a stable two-eye/one-mouth
  filled-shape identity on black, matching the approved cyan-eye/cream-mouth
  reference proportions;
- keep a calm, living idle face at 20 FPS and expose 53 expressions and states
  to trusted interaction code;
- animate each expression as a short performance with anticipation, action,
  controlled overshoot, settle, hold, and return-to-neutral timing;
- render 14 complete, boundary-safe feeding choices from compact 64x64 palette
  sprites and animate
  approach, successive bites, crumbs, chewing, straws, bubbles, and sipping;
- add sparse micro-saccades, fast-close/slow-open asymmetric blinks, damped
  shape transitions, and event-timed manga cues such as sweat, anger marks,
  tears, and bubbles;
- route every animation through a single-owner face controller with bounded
  requests, priorities, interruption rules, automatic idle recovery, and a
  restricted semantic entry point for a future AI service;
- translate deterministic conversation phases into expressions and drive mouth
  opening from the PCM audio actually sent to the speaker;
- initialize the confirmed top capacitive pad and bundled Chinese WakeNet model
  `wn9_miaomiaotongxue_tts` as local conversation wake inputs;
- capture ES8311 microphone audio as 16 kHz, signed 16-bit, mono PCM, use local
  AFE VAD for endpoint detection, and upload only the resulting utterance;
- upload microphone PCM in 100 ms frames through one chunked HTTPS
  `/conversation/stream` request while local VAD is recording; the cloud feeds
  those frames to ASR as they arrive, then performs bounded dialogue and TTS
  and returns 16 kHz PCM for ES8311/I2S playback;
- support cooperative touch barge-in during recognition, model generation, or
  playback; side touches also drive directional gaze and cloud/local reactions;
- advertise `BabyTank-XXXX` over BLE and use Espressif Network Provisioning
  Security 1 with a per-device proof-of-possession code;
- keep encrypted BLE control and diagnostic endpoints available after
  provisioning for drive/STOP, speed, light, sound, expression, volume, remote
  leases, camera discovery, game results, and hardware tests; UART commands are
  bridged to the ESP32-C2 chassis with head-side expiry and STOP preemption;
- create a WPA2 `BabyTank-XXXXXX` SoftAP for one phone, initialize the GC2145
  directly as JPEG 240x240, and serve an approximately 8 FPS MJPEG preview;
- expose local camera routes on the WPA2 SoftAP and an independently bound
  station listener on port 8081. LAN requests require a boot-scoped token
  obtained through encrypted BLE; the token remains stable across camera
  restarts within the same boot.

The display path and face renderer are implemented directly with ESP-IDF
`esp_lcd`. Faces, motion, game effects, and comic cues remain procedural. Only
the 14 food/drink objects use compact palette sprites generated from the same
approved artwork as the Android app; no animation frames are stored. It does not
include or depend on Xiaozhi, LVGL, Arduino, TFT_eSPI, third-party face code, or
third-party face code.

## AI-facing expression contract

AI output must not control pixels, eye geometry, frame timing, or system
priority. A future AI adapter should validate its structured output and call:

```c
baby_tank_face_submit_ai("curious", 70, 3200);
```

The renderer defines 53 names returned by `baby_tank_expression_name()`.
General AI replies are further restricted by the cloud emotion whitelist;
food, game, listening, processing, and other transient states are submitted by
trusted product flows. The controller clamps intensity to `20..100`, duration to
`1200..30000 ms`, assigns fixed AI priority, and returns to `calm` when the
request expires. Touch and safety code use the trusted request API at higher
priorities. See `docs/bringup/2026-10-16-ai-face-control-contract.md` for the
full boundary and suggested structured-output schema.

## Conversation-facing expression contract

The provider-independent adapter in `conversation.h/.c` accepts local session
facts (`listening`, `user_speaking`, `recognizing`, `thinking`, `ai_speaking`,
`cancelled`, `error`, and `disconnected`) and maps them to trusted face
requests. Optional model emotion metadata is still restricted to the cloud
emotion whitelist.

Speaker PCM should be passed to
`baby_tank_conversation_observe_tts_pcm16()` immediately before playback. The
adapter derives a bounded amplitude envelope; the renderer smooths it and opens
the existing cream mouth without replacing the approved character design.

The source contains an active HTTPS client for the Baby Tank Cloud device API.
Its device token and Wi-Fi credentials are loaded from NVS at runtime; no
provider key or device credential is compiled into this repository. ES8311/I2S,
WakeNet, VAD, one-request streaming cloud conversation, PCM playback, state-driven
expressions, and touch cancellation are connected and flashed to the head.
The head no longer retains a second local conversation history or calls
separate ASR, LLM, and TTS APIs.

The top capacitive pad (touch channel 1) is now a local conversation wake input.
Its interrupt only notifies a task; the task then enters `listening`, so touch
code never calls the display or network stack from interrupt context. The cloud
conversation service retains four turns of bounded, volatile short-term context
and uses the fixed “喵喵同学” robot-baby persona. History is not persisted by
the head.

Wake-up must not use cloud ASR. The intended idle path is 16 kHz microphone PCM
to a local keyword detector entirely on the ESP32-S3. Only after a local wake
event may the firmware record one utterance and call the cloud conversation
interface. The selected wake word
is the all-Chinese `喵喵同学`, using Espressif's bundled WakeNet9 model
`wn9_miaomiaotongxue_tts`. It is a fixed neural-network model, not an ASR text
match. A custom microWakeWord training pipeline is no longer required for the
first product build.
See `docs/bringup/2026-10-16-local-wakenet-plan.md` and
`docs/bringup/2026-10-16-ai-dialogue-expression-bridge.md`.

## Local camera network

The camera path uses Espressif's `esp32-camera 2.1.8` component with the board's
confirmed camera pin map. The installed GC2145 produces RGB565 frames, which
are converted to JPEG in software. The current memory profile uses one PSRAM
frame buffer with `CAMERA_GRAB_WHEN_EMPTY`; the HTTP handler keeps ownership of
the buffer until its synchronous send completes. This avoids both corrupted
rows seen with two continuous buffers under Wi-Fi load and the upstream demo's
unsafe pattern of returning a buffer while an asynchronous send still
references it.

- SoftAP SSID: the same `BabyTank-XXXXXX` identity shown over BLE.
- SoftAP password: a 16-character lowercase hex value derived from the
  per-device setup code. It is never logged or compiled into the app.
- `GET http://192.168.4.1/health`: local camera health.
- `GET http://192.168.4.1/snapshot.jpg`: one JPEG frame.
- `GET http://192.168.4.1/stream`: one-client MJPEG stream.
- `GET http://<station-ip>:8081/stream?token=<boot-token>`: authenticated LAN
  stream; the IP and token are returned only through encrypted BLE.

The SoftAP is local transport, not a second internet connection. In AP+STA mode
the robot may still use a provisioned home Wi-Fi network for cloud dialogue,
while the Android app routes only the video socket over the robot network.
Basic camera streaming has been observed on physical hardware, but sustained
frame rate, reconnect behavior, phone compatibility, and low-battery
concurrency still require regression testing. Low battery plus camera and
Wi-Fi is a known brownout risk on this board.

## PCM contract

The first product build intentionally avoids MP3/Ogg/Opus decoding. Both ASR
input and TTS output use headerless little-endian PCM with this fixed contract:

- 16,000 samples/second;
- one channel;
- signed 16-bit samples;
- 32,000 bytes/second before HTTP/TLS overhead.

The firmware rejects TTS responses with another format or sample rate instead
of sending incompatible bytes to the speaker. PCM reduces codec complexity but
costs substantially more bandwidth than Opus and does not by itself provide
jitter buffering or echo cancellation.

## Local credential image

Manufacturing identity and the per-device setup code can be placed in a local
NVS image without putting secrets in source or shell arguments. Wi-Fi fields
are optional because the Android app now provisions them over encrypted BLE:

```sh
export BABY_TANK_DEVICE_TOKEN='rotated-device-token'
export BABY_TANK_SETUP_POP='unique-device-setup-code'
tools/generate-head-credentials.sh
unset BABY_TANK_DEVICE_TOKEN BABY_TANK_SETUP_POP
```

The output is `firmware/head/build/credentials-nvs.bin`, is mode `0600`, and is
ignored by the repository. It is still plaintext NVS data and must be treated
as a secret; NVS encryption is not enabled in this development build. The
script never flashes hardware. A token previously pasted into a chat should be
rotated before real use.

## Pinned toolchain

- ESP-IDF tag: `v6.1`
- ESP-IDF commit: `fff9895c82d744c7237be8847347bdd1b07c6643`
- Target: `esp32s3`

Activate the locally installed toolchain from the repository root:

```sh
. .tools/esp-idf-v6.1/export.sh
```

Build without touching hardware:

```sh
idf.py -C firmware/head set-target esp32s3
idf.py -C firmware/head build
```

Do not flash a different unit merely because it is also an ESP32-S3. The
16 MiB Flash and 8 MiB octal PSRAM settings were confirmed only on the device
recorded in `docs/bringup/2026-10-16-head-usb.md`.

Before flashing, verify that the original full-Flash backup exists and passes
its recorded SHA-256 check. A full restore overwrites device data and is not an
ordinary build step.
