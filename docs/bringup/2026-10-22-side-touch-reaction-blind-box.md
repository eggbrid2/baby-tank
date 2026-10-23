# Side-touch reaction blind box

Date: 2026-10-22

Firmware: `0.14.68`

## Behaviour

- Left and right pads use independent six-clip personality pools.
- A normal reaction has a 1-in-12 chance of drawing from four shared surprise
  clips.
- The same side pool and the surprise pool do not repeat their immediately
  previous selection.
- The first two touches in a burst remain playful. The third, fourth and fifth
  touches use the bounded warning, annoyed and angry escalation.
- Eight seconds without a valid side touch resets the burst.
- At most two additional side reactions may wait behind the currently playing
  clip. Further touches are dropped instead of creating a long audio backlog.
- Selection and playback are entirely local. They do not require Wi-Fi, ASR,
  an LLM or cloud TTS at runtime.

## Assets and memory cost

Sixteen PCM16 mono 16 kHz clips were generated using the configured production
voice. The generation script forces a slightly faster rate for short toy-like
reactions. Source phrases remain reproducible in
`tools/generate-touch-clips.mjs`.

The resulting firmware image is `0x432ce0` bytes in a `0x600000` application
partition, leaving `0x1cd320` bytes (30%). This is sufficient for this release,
but uncompressed PCM should not be treated as an unlimited content store.
Future large reaction packs need either a compressed local format, a dedicated
asset partition, or selective download/cache management.

## Verification

- `git diff --check`: passed.
- ESP-IDF 6.1 `idf.py build`: passed.
- Image flashed over USB without erasing NVS.
- Physical left/right selection, non-repeat behaviour and rapid-touch
  escalation require a human touch pass because capacitive input cannot be
  meaningfully validated by a software-only trigger.
