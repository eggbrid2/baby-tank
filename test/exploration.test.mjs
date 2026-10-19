import test from 'node:test';
import assert from 'node:assert/strict';
import { explicitExploreIntent, parseVisionStep } from '../src/exploration.mjs';

test('exploration needs a direct utterance, not a mention or negation', () => {
  for (const utterance of ['去看看前面有什么', '喵喵同学去探索一下', '看看周围有什么']) {
    assert.equal(explicitExploreIntent(utterance), true);
  }
  for (const utterance of ['不要去看看', '如果去看看会怎样', '它说去探索一下',
    '你可以去看看吗', '为什么要探索', '去看看前面有什么然后一直前进']) {
    assert.equal(explicitExploreIntent(utterance), false);
  }
});

test('vision proposal is schema- and budget-bounded', () => {
  const step = { move: 'left', durationMs: 3000, lightMode: 'rainbow',
    emotion: 'curious', intensity: 60, description: '左侧较开阔' };
  assert.deepEqual(parseVisionStep(JSON.stringify(step), { usedSteps: 1, usedMs: 9000 }), step);
  assert.equal(parseVisionStep(JSON.stringify(step), { usedSteps: 3, usedMs: 0 }), null);
  assert.equal(parseVisionStep(JSON.stringify(step), { usedSteps: 1, usedMs: 13_000 }), null);
  assert.equal(parseVisionStep(JSON.stringify({ ...step, durationMs: 1000 }), { usedSteps: 0, usedMs: 0 }), null);
  assert.equal(parseVisionStep(JSON.stringify({ ...step, move: 'up' }), { usedSteps: 0, usedMs: 0 }), null);
  assert.equal(parseVisionStep(JSON.stringify({ ...step, lightMode: 'red' }), { usedSteps: 0, usedMs: 0 }), null);
  assert.equal(parseVisionStep(JSON.stringify({ ...step, durationMs: 0, move: 'stop' }),
    { usedSteps: 3, usedMs: 15000 }).move, 'stop');
});
