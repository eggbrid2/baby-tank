import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { Readable } from 'node:stream';

const [, , wavPath, repeatArg = '1', mode = 'batch'] = process.argv;
const token = process.env.BABY_TANK_DEVICE_TOKEN;
const repeat = Number(repeatArg);
if (!wavPath || !token || !Number.isInteger(repeat) || repeat < 1 || repeat > 5 ||
    !['batch', 'stream'].includes(mode)) {
  throw new Error('usage: BABY_TANK_DEVICE_TOKEN=<secret> node tools/probe-cloud-conversation.mjs input.wav [1..5] [batch|stream]');
}

const wave = readFileSync(wavPath);
if (wave.toString('ascii', 0, 4) !== 'RIFF' || wave.toString('ascii', 8, 12) !== 'WAVE') {
  throw new Error('input is not RIFF/WAVE');
}
let audio;
let formatValid = false;
for (let offset = 12; offset + 8 <= wave.length;) {
  const kind = wave.toString('ascii', offset, offset + 4);
  const size = wave.readUInt32LE(offset + 4);
  const start = offset + 8;
  if (start + size > wave.length) throw new Error('truncated WAVE chunk');
  if (kind === 'fmt ') {
    formatValid = size >= 16 && wave.readUInt16LE(start) === 1 &&
      wave.readUInt16LE(start + 2) === 1 && wave.readUInt32LE(start + 4) === 16000 &&
      wave.readUInt16LE(start + 14) === 16;
  }
  if (kind === 'data') audio = wave.subarray(start, start + size);
  offset = start + size + (size % 2);
}
if (!formatValid || !audio || audio.length < 3200 || audio.length % 2 !== 0) {
  throw new Error('WAVE must be mono 16 kHz PCM S16LE with at least 100 ms of audio');
}

for (let index = 0; index < repeat; ++index) {
  const startedAt = performance.now();
  async function* microphoneFrames() {
    for (let offset = 0; offset < audio.length; offset += 3_200) {
      yield audio.subarray(offset, Math.min(offset + 3_200, audio.length));
      if (offset + 3_200 < audio.length) await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  const response = await fetch(`https://www.xydocshare.com/api/device/v1/conversation${mode === 'stream' ? '/stream' : ''}?format=pcm&sampleRate=16000&diagnostic=1`, {
    method: 'POST',
    headers: { authorization: `Bearer ${token}`, 'content-type': 'audio/pcm' },
    body: mode === 'stream' ? Readable.from(microphoneFrames()) : audio,
    ...(mode === 'stream' ? { duplex: 'half' } : {}),
    signal: AbortSignal.timeout(180_000),
  });
  const headersAt = performance.now();
  if (!response.ok) {
    const body = await response.text();
    console.log(JSON.stringify({ run: index + 1, mode, status: response.status,
      beforeHeadersMs: Math.round(headersAt - startedAt), error: body.slice(0, 200) }));
    continue;
  }
  const reader = response.body.getReader();
  const chunks = [];
  let firstAt = 0;
  let previousAt = 0;
  let lastAt = 0;
  let maxDownloadGapMs = 0;
  let downloadGapsOver100Ms = 0;
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    const arrivedAt = performance.now();
    if (!firstAt) firstAt = arrivedAt;
    if (previousAt) {
      const gap = arrivedAt - previousAt;
      maxDownloadGapMs = Math.max(maxDownloadGapMs, gap);
      if (gap > 100) ++downloadGapsOver100Ms;
    }
    previousAt = arrivedAt;
    lastAt = arrivedAt;
    chunks.push(Buffer.from(value));
  }
  const answer = Buffer.concat(chunks);
  let nonzero = 0;
  for (let offset = 0; offset + 1 < answer.length; offset += 2) {
    if (answer.readInt16LE(offset) !== 0) ++nonzero;
  }
  console.log(JSON.stringify({ run: index + 1, mode, status: response.status,
    traceId: response.headers.get('x-turn-trace-id'),
    serverTiming: response.headers.get('server-timing'),
    upstreamTtsMaxGapMs: Number(response.headers.get('x-turn-tts-max-gap-ms')),
    upstreamTtsStreamMs: Number(response.headers.get('x-turn-tts-stream-ms')),
    beforeHeadersMs: Math.round(headersAt - startedAt),
    firstBodyMs: Math.round(firstAt - startedAt),
    bodyDownloadMs: Math.round(lastAt - firstAt),
    totalMs: Math.round(performance.now() - startedAt),
    downloadChunks: chunks.length,
    maxDownloadGapMs: Math.round(maxDownloadGapMs), downloadGapsOver100Ms,
    pcmBytes: answer.length, pcmDurationMs: Math.round(answer.length / 32),
    pcmValid: response.headers.get('x-audio-format') === 'pcm' &&
      response.headers.get('x-audio-sample-rate') === '16000' &&
      answer.length > 0 && answer.length % 2 === 0 && nonzero > 0,
  }));
}
