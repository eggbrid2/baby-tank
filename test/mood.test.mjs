import test from 'node:test';
import assert from 'node:assert/strict';
import { DEFAULT_MOOD, advanceMood, moodHeaders, moodPrompt, parseThought } from '../src/mood.mjs';

test('mood keeps a slow bounded state while a reply supplies a short affect pulse', () => {
  const first = advanceMood(DEFAULT_MOOD, 'happy', 80,
    { key: 'star', text: '想把亮晶晶的开心收好', displayText: '亮晶晶' },
    new Date('2026-10-19T00:00:00.000Z'));
  assert.ok(first.valence > DEFAULT_MOOD.valence);
  assert.ok(first.arousal > DEFAULT_MOOD.arousal);
  assert.equal(first.thought.key, 'star');
  for (const field of ['valence', 'arousal', 'dominance']) {
    assert.ok(first[field] >= -100 && first[field] <= 100);
  }

  const later = advanceMood(first, 'calm', 20, null,
    new Date('2026-10-19T02:00:00.000Z'));
  assert.ok(Math.abs(later.valence - 25) < Math.abs(first.valence - 25));
  assert.match(moodPrompt(later), /不是真实意识/);
});

test('thought output is allowlisted and headers contain only compact device state', () => {
  assert.deepEqual(parseThought({ key: 'not-an-icon', text: '任意图案' }, 'curious'),
    { key: 'question', text: '刚才那件事到底是什么呢', displayText: '想一想' });
  assert.deepEqual(parseThought({ key: 'moon', text: '\u0000做个小梦' }, 'sleepy'),
    { key: 'moon', text: '做个小梦', displayText: '做个好梦' });
  const headers = moodHeaders({ ...DEFAULT_MOOD,
    thought: { key: 'fish', text: '想和小鱼比赛吐泡泡', displayText: '小鱼游' } });
  assert.equal(headers['x-ai-thought'], 'fish');
  assert.equal(Buffer.from(headers['x-ai-thought-display-b64'], 'base64url').toString(), '小鱼游');
  assert.equal(headers['x-ai-mood'], 'calm');
  assert.equal(Object.values(headers).some((value) => /[\p{Script=Han}]/u.test(value)), false);
});
