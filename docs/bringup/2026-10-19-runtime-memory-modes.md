# Runtime memory modes

Date: 2026-10-19

Firmware version: `baby_tank_head 0.13.33`

## Decision

The head no longer keeps the camera stream and the complete ESP-SR voice
runtime active at the same time. The existing Android controller still sends
drive, light, speed and mode commands over BLE, so BLE remains active in remote
mode until those commands have a Wi-Fi transport.

## Mode behaviour

- AI mode owns AFE, WakeNet, VAD and the feed/fetch/dialogue tasks.
- Remote mode destroys the voice runtime before starting the GC2145 and its
  HTTP stream.
- Leaving remote mode stops both camera HTTP servers, deinitializes the camera
  and JPEG encoder, then recreates the voice runtime.
- A renewable five-second remote lease automatically restores AI mode if the
  app disappears without sending the exit command.
- AI vision initializes the camera only for the requested snapshot and releases
  it immediately afterwards.

## Expected memory effect

The measured voice initialization delta before this change was 44,252 bytes of
internal heap. Camera initialization was approximately 20–27 KiB. These values
are not added together because the new modes intentionally make the two
subsystems mutually exclusive. Runtime logs now report the actual number of
internal SRAM bytes released by each voice shutdown.

## Verification

- ESP-IDF build completed successfully.
- Output: `firmware/head/build/baby_tank_head.bin`
- No device flash was performed in this change.
- Hardware verification must cover repeated AI → remote → AI transitions,
  an active MJPEG client during exit, remote lease expiry, and AI vision capture.

## Known constraint

BLE cannot yet be released in Wi-Fi video remote mode because the Android app
has no equivalent authenticated Wi-Fi control endpoint. Disabling BLE now
would save memory but would also break steering and remote-mode keepalive.
