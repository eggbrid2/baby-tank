# Baby Tank Android

Native Android companion app for Baby Tank. The current build targets Android
8.0+ and uses Jetpack Compose.

## Implemented now

- cute monochrome robot-baby home, pairing, control, hardware-lab, and
  personality screens;
- runtime BLE permissions and discovery of `BabyTank-XXXX` devices;
- Espressif Network Provisioning `Security 1` using the personal test device
  secret from ignored `local.properties`;
- current Wi-Fi SSID detection, nearby Wi-Fi scan, manual password submission,
  and device-side connection verification;
- encrypted `baby-control` STOP handshake against the chassis UART echo;
- Android 10+ system-mediated connection to the robot's WPA2
  `BabyTank-XXXXXX` SoftAP, using the local test configuration to derive the
  password without storing it;
- 240x240 MJPEG camera preview with frame-rate display, bounded frame parsing,
  disconnect handling, and three reconnect attempts;
- four-direction BLE controls with a 40%-100% speed slider, defaulting to
  100%; the slider has its own fixed band below the camera instead of covering
  video; 60% has been physically verified while higher settings still require
  grounded load testing;
- repeated commands while dragging, STOP on release or app backgrounding,
  an emergency-stop button, and head-side 300 ms command expiry.

The app never needs the cloud device token. This personal debug build embeds a
local proof-of-possession value at build time; APK contents can be extracted,
so this is not a production credential design.

## Build

```sh
cd android
JAVA_HOME='/Applications/Android Studio.app/Contents/jbr/Contents/Home' \
  ./gradlew :app:assembleDebug
```

Output: `app/build/outputs/apk/debug/app-debug.apk`.

## Transport boundary

- BLE: provisioning, drive/stop/light commands, and low-rate telemetry.
- Local Wi-Fi or a phone hotspot: camera preview, logs, OTA, and other bulk
  transfer. Local Wi-Fi does not require an internet connection.
- Internet: cloud ASR/LLM/TTS only. Basic driving must remain usable offline.

Camera video is intentionally not transported over BLE. A phone can decode,
record, and run recognition on frames, but it cannot compensate for BLE's
limited link throughput. In field mode, either the robot creates a temporary
Wi-Fi network or it joins the phone's hotspot.

## Camera test flow

1. Discover and connect the same `BabyTank-XXXXXX` device over BLE.
2. Open Control. Camera startup is independent of the chassis STOP handshake.
3. The app first requests the boot-scoped camera token over encrypted BLE and
   tries the robot's port 8081 address when the phone is on the same LAN.
4. If the phone is not on that subnet, the app asks the robot to enable its
   SoftAP and requests the WPA2 network through `WifiNetworkSpecifier`. On
   Android 10+, accept the operating-system Wi-Fi connection dialog once.
5. The video socket is bound to the selected Android `Network`; the app process
   is not globally rebound, so cellular/cloud traffic keeps its normal route.

Android 8 and 9 cannot use `WifiNetworkSpecifier`; connect to the robot network
in system Wi-Fi settings first, then return to the app. HTTP cleartext is denied
globally except for the fixed robot SoftAP address. The video endpoint itself
has no HTTP login because WPA2 is the access boundary, and firmware rejects
requests arriving through the home-Wi-Fi interface.

## Safety gate

The 8-byte BLE control packet is versioned and sequence-numbered. The head
rejects motor values outside ±1000‰ and requires an exact chassis STOP echo
before motion. The joystick offers only the four cardinal directions tested
on the lifted tracks. Releasing the speed slider sends STOP so a stale command
cannot remain active. BLE drives and AI timed actions share one UART sender;
BLE/STOP preempts AI. The head expires unrefreshed movement after 300 ms.
The installed chassis source appears to have its own roughly 500 ms loss-of-
command stop, but that independent behavior has **not** yet been physically
validated by disconnecting the head. Keep the tracks lifted during testing;
do not treat this debug build as an unattended child-safe release.
