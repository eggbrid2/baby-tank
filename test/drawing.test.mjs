import test from 'node:test';
import assert from 'node:assert/strict';
import { DRAWING_DISPLAY_MS, parseSvgDrawing, rasterizeDrawing,
  sanitizeDrawingSvg } from '../src/drawing.mjs';

const svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 240">' +
  '<rect x="0" y="0" width="240" height="240" fill="#071018"/>' +
  '<rect x="40" y="95" width="160" height="70" rx="16" fill="#59f3e8"/>' +
  '<circle cx="78" cy="172" r="20" fill="#fff1c7"/>' +
  '<circle cx="162" cy="172" r="20" fill="#fff1c7"/></svg>';

function assertBaselineJpeg(jpeg) {
  assert.deepEqual([...jpeg.subarray(0, 2)], [0xff, 0xd8]);
  const beforeScan = jpeg.subarray(0, jpeg.indexOf(Buffer.from([0xff, 0xda])));
  assert.ok(beforeScan.indexOf(Buffer.from([0xff, 0xc0])) >= 0,
    'ESP32 needs a baseline SOF0 frame');
  assert.equal(beforeScan.indexOf(Buffer.from([0xff, 0xc2])), -1,
    'progressive SOF2 frames fail on the head');
}

test('model-authored SVG becomes a bounded head-decodable JPEG', async () => {
  const drawing = parseSvgDrawing(JSON.stringify({ drawing: {
    durationMs: DRAWING_DISPLAY_MS, svg,
  } }));
  assert.deepEqual(drawing, { durationMs: DRAWING_DISPLAY_MS, svg });
  const rendered = await rasterizeDrawing(drawing);
  assertBaselineJpeg(rendered.jpeg);
  assert.equal(rendered.durationMs, DRAWING_DISPLAY_MS);
  assert.ok(rendered.jpeg.length < 96 * 1024);
});

test('drawing refuses icons, active SVG content, malformed nesting and blank art', async () => {
  assert.equal(parseSvgDrawing(JSON.stringify({ drawing: {
    durationMs: DRAWING_DISPLAY_MS,
    items: [{ emoji: '🚗', x: 120, y: 120, size: 180 }],
  } })), null);
  assert.throws(() => sanitizeDrawingSvg(svg.replace('<rect x="40"',
    '<image href="https://example.com/x" x="40"')), /drawing_svg_unsafe/);
  assert.throws(() => sanitizeDrawingSvg(svg.replace('</svg>', '</g></svg>')),
    /drawing_svg_close_invalid/);
  const blank = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 240">' +
    '<rect x="0" y="0" width="240" height="240" fill="#071018"/>' +
    '<circle cx="30" cy="30" r="5" fill="#59f3e8"/>' +
    '<circle cx="50" cy="30" r="5" fill="#fff1c7"/>' +
    '<circle cx="70" cy="30" r="5" fill="#ff8fb8"/></svg>';
  await assert.rejects(rasterizeDrawing({ svg: blank }),
    /drawing_subject_visibility_invalid/);
});
