// Deterministic, short two-note vehicle horn for the head's 16 kHz mono DAC.
// It lives in flash; playback does not allocate a PCM buffer in internal RAM.
import { writeFileSync } from 'node:fs';

const sampleRate = 16000;
const durationSeconds = 0.7;
const samples = new Int16Array(Math.round(sampleRate * durationSeconds));
const bursts = [[0.02, 0.22], [0.34, 0.64]];

for (let i = 0; i < samples.length; i++) {
  const t = i / sampleRate;
  const burst = bursts.find(([start, end]) => t >= start && t < end);
  if (!burst) continue;
  const [start, end] = burst;
  const attack = Math.min(1, (t - start) / 0.018);
  const release = Math.min(1, (end - t) / 0.028);
  const envelope = Math.max(0, Math.min(attack, release));
  const horn = 0.52 * Math.sin(2 * Math.PI * 480 * t) +
    0.43 * Math.sin(2 * Math.PI * 603 * t) +
    0.14 * Math.sin(2 * Math.PI * 960 * t) +
    0.10 * Math.sin(2 * Math.PI * 1206 * t);
  samples[i] = Math.round(15500 * envelope * horn);
}

writeFileSync('firmware/head/assets/car-horn.pcm', Buffer.from(samples.buffer));
process.stdout.write(`car-horn: ${samples.length / sampleRate * 1000} ms\n`);
