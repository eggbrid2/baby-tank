import test from 'node:test';
import assert from 'node:assert/strict';
import { conversationMessages, explicitDrawIntent, explicitDriveIntent, explicitLookIntent, explicitSleepIntent,
  parseConversationReply, parseDrawingProposal } from '../src/conversation.mjs';

test('camera attachment requires a visual ASR intent', () => {
  for (const utterance of ['看看眼前是什么', '眼前是什么', '前面有什么',
    '你看到了什么', '摄像头里有什么']) {
    assert.equal(explicitLookIntent(utterance), true, utterance);
  }
  for (const utterance of ['今天想玩什么', '你往后走一点', '画个小汽车',
    '不要看眼前是什么', '你怎么看待这个故事']) {
    assert.equal(explicitLookIntent(utterance), false, utterance);
  }
});

test('conversation parser keeps model action proposals strictly bounded', () => {
  const prompt = conversationMessages('前进一步', [], {});
  assert.match(prompt[0].content, /action/);
  assert.match(prompt[0].content, /自主判断移动是否/);

  const forward = parseConversationReply(JSON.stringify({ reply: '好', emotion: 'happy', intensity: 60,
    action: { type: 'drive', direction: 'forward', durationMs: 2000 } }));
  assert.deepEqual(forward.action, { status: 'shadow', type: 'drive', direction: 'forward',
    speedPerMille: 900, durationMs: 2000, lightMode: 'unchanged', reason: '' });
  const lit = parseConversationReply(JSON.stringify({ reply: '出发', action: {
    type: 'drive', direction: 'left', speedPerMille: 1000,
    durationMs: 2500, lightMode: 'rainbow',
  } }));
  assert.equal(lit.action.lightMode, 'rainbow');
  assert.equal(lit.action.speedPerMille, 1000);

  for (const action of [
    { type: 'drive', direction: 'forward', durationMs: 2000, speed: 1000 },
    { type: 'drive', direction: 'forward', durationMs: 2000, speedPerMille: 650 },
    { type: 'drive', direction: 'forward', durationMs: 2000, speedPerMille: 1001 },
    { type: 'drive', direction: 'forward', durationMs: 2000, lightMode: 'blue' },
    { type: 'drive', direction: 'forward', durationMs: 10000 },
    { type: 'drive', direction: 'up' },
    { type: 'camera', direction: 'forward' },
    [{ type: 'drive', direction: 'forward', durationMs: 2000 }],
    'drive_forward',
  ]) {
    const reply = parseConversationReply(JSON.stringify({ reply: '好', action }));
    assert.equal(reply.action.status, 'rejected');
    assert.equal(reply.action.type, 'none');
  }
  assert.equal(parseConversationReply('{"reply":"聊聊而已","action":null}').action.status, 'none');
  assert.equal(parseConversationReply('普通文本').action.status, 'none');

  const expressive = parseConversationReply(JSON.stringify({
    reply: '咦，是什么？原来是小花！', emotion: 'calm', intensity: 50,
    speechSegments: [
      { text: '咦，是什么？', emotion: 'curious', intensity: 62 },
      { text: '原来是小花！', emotion: 'happy', intensity: 78 },
    ], action: null,
  }));
  assert.deepEqual(expressive.speechSegments, [
    { text: '咦，是什么？', emotion: 'curious', intensity: 62 },
    { text: '原来是小花！', emotion: 'happy', intensity: 78 },
  ]);
  assert.equal(expressive.emotion, 'curious');
  assert.equal(expressive.speechSegmentSource, 'model');
  const spaced = parseConversationReply(JSON.stringify({ reply: '咦？ 真漂亮！',
    speechSegments: [
      { text: '咦？', emotion: 'curious', intensity: 60 },
      { text: '真漂亮！', emotion: 'happy', intensity: 80 },
    ] }));
  assert.equal(spaced.speechSegments.length, 2);
  assert.equal(spaced.speechSegmentSource, 'model');
  assert.equal(parseConversationReply(JSON.stringify({ reply: '好',
    speechSegments: [{ text: '不一致', emotion: 'happy', intensity: 70 }] }))
    .speechSegments.length, 1);
});

test('AI drawing accepts only layouts backed by bundled designer SVG assets', () => {
  assert.equal(explicitDrawIntent('画辆自行车'), true);
  assert.equal(explicitDrawIntent('画画吧'), true);
  assert.equal(explicitDrawIntent('给我画一幅画'), true);
  assert.equal(explicitDrawIntent('你会画画吗'), false);
  assert.equal(explicitDrawIntent('不要画小汽车'), false);
  const items = [{ emoji: '🐈', x: 98, y: 126, size: 150, rotation: 0, flipX: false },
    { emoji: '🧶', x: 190, y: 158, size: 62, rotation: 0, flipX: false }];
  assert.deepEqual(parseDrawingProposal(`\`\`\`json\n${JSON.stringify({ drawing: {
    durationMs: 6000, items,
  } })}\n\`\`\``), { durationMs: 6000, items: [
    { iconName: 'cat', ...items[0] }, { iconName: 'yarn', ...items[1] },
  ] });
  assert.equal(parseDrawingProposal(JSON.stringify({ drawing: {
    durationMs: 6000, items: [{ emoji: 'A', x: 120, y: 120, size: 180 }],
  } })), null);
  assert.equal(parseDrawingProposal(JSON.stringify({ drawing: {
    durationMs: 6000, items: [{ emoji: '🐈', x: 30, y: 120, size: 180 }],
  } })), null);
  assert.equal(parseConversationReply(JSON.stringify({ reply: '画好了',
    drawing: { durationMs: 6000, items } })).drawing, null,
  'the dialogue model cannot bypass the dedicated curated renderer');
});

test('explicit drive grammar stays narrow as the deterministic fallback', () => {
  for (const [utterance, direction, durationMs] of [
    ['向前走一步', 'forward', null], ['请你后退一下', 'backward', null],
    ['喵喵同学左转一下', 'left', null], ['坦克向右转', 'right', null],
    ['前进3秒', 'forward', 3000], ['向右转2.5秒', 'right', 2500],
    ['向前走两秒', 'forward', 2000], ['后退三秒吧', 'backward', 3000],
    ['左转一下吧', 'left', null],
    ['让你往后走一点', 'backward', null],
  ]) assert.deepEqual(explicitDriveIntent(utterance), { direction, durationMs });
  for (const utterance of ['为什么不能前进？', '不要向前走', '如果向前走会怎样',
    '先向前再向后', '我说向左转了吗', '向前走十米', '前进1秒',
    '不要让你往后走一点', '我说让你往后走一点吗']) {
    assert.equal(explicitDriveIntent(utterance), null);
  }
});

test('an exploration reply is short and does not promise an unseen scene', () => {
  const messages = conversationMessages('去看看前面有什么', [], {});
  assert.match(messages[2].content, /不超过 18 个汉字/);
  assert.match(messages[2].content, /不要提前宣称看到了什么/);
  assert.equal(messages.at(-1).content, '去看看前面有什么');
});

test('only a standalone rest command ends the ongoing voice session', () => {
  for (const utterance of ['你休息吧', '喵喵同学，关闭对话', '不聊了', '关闭']) {
    assert.equal(explicitSleepIntent(utterance), true);
  }
  for (const utterance of ['为什么要休息？', '关闭摄像头', '不要关闭', '说声再见', '先休息再前进']) {
    assert.equal(explicitSleepIntent(utterance), false);
  }
  assert.match(conversationMessages('你休息吧', [], {})[2].content, /简短道别/);
});
