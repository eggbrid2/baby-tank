import assert from 'node:assert/strict';
import test from 'node:test';
import sharp from 'sharp';

import { normalizeSketchJpeg, parseSketchGuess } from '../src/sketch_guess.mjs';

test('sketch guess accepts one bounded noun and rejects extra model fields', () => {
  assert.equal(parseSketchGuess('```json\n{"guess":"小飞机"}\n```'), '小飞机');
  assert.equal(parseSketchGuess('{"guess":""}'), null);
  assert.equal(parseSketchGuess('{"guess":"这是一个特别特别特别特别长的答案"}'), null);
  assert.equal(parseSketchGuess('{"guess":"小猫","reason":"耳朵"}'), null);
});

test('sketch normalizer accepts visible strokes and rejects blank canvas', async () => {
  const visible = await sharp({ create: { width: 512, height: 512, channels: 3,
    background: '#fffdf7' } }).composite([{ input: Buffer.from(
      '<svg width="512" height="512"><path d="M80 400 L256 80 L432 400 Z" ' +
      'fill="none" stroke="#17242d" stroke-width="18"/></svg>') }])
    .jpeg().toBuffer();
  const normalized = await normalizeSketchJpeg(visible);
  assert.deepEqual([...normalized.subarray(0, 2)], [0xff, 0xd8]);

  const blank = await sharp({ create: { width: 512, height: 512, channels: 3,
    background: '#fffdf7' } }).jpeg().toBuffer();
  await assert.rejects(() => normalizeSketchJpeg(blank), /sketch_ink_invalid/);
});
