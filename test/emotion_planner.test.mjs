import test from 'node:test';
import assert from 'node:assert/strict';
import { emotionPlanMessages, parseEmotionPlan, splitSpeechPhrases } from '../src/emotion_planner.mjs';

test('speech phrases retain punctuation and stay within three TTS segments', () => {
  assert.deepEqual(splitSpeechPhrases('咦？原来是小花！'), ['咦？', '原来是小花！']);
  assert.deepEqual(splitSpeechPhrases('我先看看，原来是一只小猫'),
    ['我先看看，', '原来是一只小猫']);
  assert.deepEqual(splitSpeechPhrases('甲。乙。丙。丁。'), ['甲。', '乙。', '丙。丁。']);
  assert.deepEqual(splitSpeechPhrases('你好。'), ['你好。']);
});

test('emotion labels cannot change words or become actions', () => {
  const phrases = ['咦？', '太好了！'];
  assert.match(emotionPlanMessages(phrases)[0].content, /不执行动作/);
  assert.deepEqual(parseEmotionPlan('{"emotions":["curious","happy"],"intensities":[60,80]}', phrases), [
    { text: '咦？', emotion: 'curious', intensity: 60 },
    { text: '太好了！', emotion: 'happy', intensity: 80 },
  ]);
  assert.equal(parseEmotionPlan('{"emotions":["curious"],"intensities":[60]}', phrases), null);
  assert.equal(parseEmotionPlan('{"emotions":["drive","happy"],"intensities":[60,80]}', phrases), null);
});
