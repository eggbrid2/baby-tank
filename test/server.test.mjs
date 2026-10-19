import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { once } from 'node:events';
import { EventEmitter } from 'node:events';
import { Readable } from 'node:stream';
import { DatabaseSync } from 'node:sqlite';
import WebSocket from 'ws';
import { createApp } from '../src/server.mjs';

test('admin, provider, device and safe OTA flow', async (t) => {
  const nativeFetch = globalThis.fetch;
  let upstreamAuthorization = '';
  const llmRequests = [];
  let llmReply = '你好';
  let drawingProposalReply = null;
  let asrText = '测试成功';
  let ttsApiKey = '';
  let ttsRequest = null;
  let holdTtsSecondChunk = false;
  let releaseTtsSecondChunk = null;
  let visionReply = JSON.stringify({ move: 'left', durationMs: 3000,
    lightMode: 'rainbow', emotion: 'curious', intensity: 60,
    description: '左侧可以观察' });
  const visionRequests = [];
  globalThis.fetch = async (input, init) => {
    if (String(input) === 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions') {
      const request = JSON.parse(init.body);
      visionRequests.push(request);
      assert.equal(init.headers.authorization, 'Bearer ali-asr-key');
      return new Response(JSON.stringify({ choices: [{ message: { content: visionReply } }] }),
        { status: 200, headers: { 'content-type': 'application/json' } });
    }
    if (String(input) === 'https://example.com/v1/chat/completions') {
      upstreamAuthorization = init.headers.authorization;
      const request = JSON.parse(init.body);
      llmRequests.push(request);
      const content = drawingProposalReply &&
        request.messages[0]?.content.includes('Fluent Emoji SVG')
        ? drawingProposalReply : llmReply;
      return new Response(JSON.stringify({ id: 'mock', model: request.model, choices: [{ message: { role: 'assistant', content } }] }),
        { status: 200, headers: { 'content-type': 'application/json' } });
    }
    if (String(input) === 'https://openspeech.bytedance.com/api/v3/tts/unidirectional/sse') {
      const headers = new Headers(init.headers);
      ttsApiKey = headers.get('x-api-key');
      ttsRequest = JSON.parse(init.body);
      const first = Buffer.from('mock-');
      const second = Buffer.from('audio');
      if (holdTtsSecondChunk) {
        return new Response(new ReadableStream({
          start(controller) {
            controller.enqueue(Buffer.from(`data: ${JSON.stringify({ code: 20000000, data: first.toString('base64') })}\n\n`));
            releaseTtsSecondChunk = () => {
              controller.enqueue(Buffer.from(`data: ${JSON.stringify({ code: 20000000, data: second.toString('base64') })}\n\n`));
              controller.close();
            };
          },
        }), { status: 200, headers: { 'content-type': 'text/event-stream' } });
      }
      return new Response(`data: ${JSON.stringify({ code: 20000000, data: first.toString('base64') })}\n\ndata: ${JSON.stringify({ code: 20000000, data: second.toString('base64') })}\n\n`,
        { status: 200, headers: { 'content-type': 'text/event-stream' } });
    }
    return nativeFetch(input, init);
  };

  let asrAuthorization = '';
  let asrAudioBytes = 0;
  class MockAsrSocket extends EventEmitter {
    constructor(_url, options) {
      super();
      asrAuthorization = options.headers.Authorization;
      queueMicrotask(() => this.emit('open'));
    }

    send(data) {
      if (Buffer.isBuffer(data) || data instanceof Uint8Array) {
        asrAudioBytes += data.length;
        return;
      }
      const event = JSON.parse(data);
      if (event.header.action === 'run-task') {
        queueMicrotask(() => this.emit('message', Buffer.from(JSON.stringify({ header: { event: 'task-started' }, payload: {} }))));
      } else if (event.header.action === 'finish-task') {
        queueMicrotask(() => {
          this.emit('message', Buffer.from(JSON.stringify({ header: { event: 'result-generated' }, payload: { output: { sentence: {
            text: asrText, sentence_end: true, begin_time: 0, end_time: 600,
          } } } })));
          this.emit('message', Buffer.from(JSON.stringify({ header: { event: 'task-finished' }, payload: {} })));
        });
      }
    }

    close() {}
  }
  const dataDir = mkdtempSync(join(tmpdir(), 'baby-tank-cloud-'));
  const config = {
    host: '127.0.0.1', port: 0, dataDir, dbPath: join(dataDir, 'db.sqlite3'), firmwareDir: join(dataDir, 'firmware'),
    adminUsername: 'admin', adminPassword: 'correct-horse-battery-staple', masterKey: Buffer.alloc(32, 7),
    cookieSecure: false, publicBaseUrl: '', firmwareMaxBytes: 1024 * 1024, speechMaxBytes: 1024 * 1024,
    ttsMaxCharacters: 2000, asrFrameDelayMs: 0, webSocketFactory: (url, options) => new MockAsrSocket(url, options),
  };
  const { server } = createApp(config);
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  t.after(() => { globalThis.fetch = nativeFetch; server.close(); rmSync(dataDir, { recursive: true, force: true }); });
  const base = `http://127.0.0.1:${server.address().port}`;

  for (const route of ['/account', '/account/']) {
    const accountPage = await fetch(`${base}${route}`);
    assert.equal(accountPage.status, 200);
    assert.match(await accountPage.text(), /修改管理员密码/);
  }

  const login = await fetch(`${base}/api/auth/login`, { method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ username: 'admin', password: config.adminPassword }) });
  assert.equal(login.status, 200);
  const auth = await login.json();
  const cookie = login.headers.get('set-cookie').split(';')[0];
  const adminHeaders = { cookie, 'x-csrf-token': auth.csrfToken, 'content-type': 'application/json' };

  const provider = await fetch(`${base}/api/admin/providers`, { method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ kind: 'llm', provider: 'qwen', label: 'Qwen', enabled: true, baseUrl: 'https://example.com/v1', model: 'qwen-test',
      credentials: { apiKey: 'plaintext-credential-sentinel-937451' },
      options: { thinkingMode: 'on', reasoningEffort: 'medium', temperature: '0.7', maxTokens: '2048' } }) });
  assert.equal(provider.status, 200);
  for (const path of [config.dbPath, `${config.dbPath}-wal`].filter(existsSync)) {
    assert.equal(readFileSync(path).includes(Buffer.from('plaintext-credential-sentinel-937451')), false, 'provider credential must not be stored in plaintext');
  }

  const enrollment = await fetch(`${base}/api/admin/devices`, { method: 'POST', headers: adminHeaders,
    body: JSON.stringify({ deviceId: 'tank-001', name: 'Test Tank', board: 'baby-tank-s3' }) });
  assert.equal(enrollment.status, 201);
  const { token } = await enrollment.json();
  const deviceHeaders = { authorization: `Bearer ${token}` };

  const configResponse = await fetch(`${base}/api/device/v1/config`, { headers: deviceHeaders });
  assert.equal(configResponse.status, 200);
  const deviceConfig = await configResponse.json();
  assert.equal(deviceConfig.services.llm.provider, 'qwen');
  assert.equal(JSON.stringify(deviceConfig).includes('secret'), false);

  const llm = await fetch(`${base}/api/device/v1/llm`, { method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ messages: [{ role: 'user', content: '你好' }] }) });
  assert.equal(llm.status, 200);
  assert.equal((await llm.json()).model, 'qwen-test');
  assert.equal(upstreamAuthorization, 'Bearer plaintext-credential-sentinel-937451');
  assert.equal(llmRequests[0].reasoning_effort, 'medium');
  assert.equal(llmRequests[0].max_tokens, 2048);
  assert.equal('temperature' in llmRequests[0], false, 'temperature must be omitted while reasoning is enabled');

  const deepseekProvider = await fetch(`${base}/api/admin/providers`, { method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ kind: 'llm', provider: 'deepseek', label: 'DeepSeek', enabled: true, baseUrl: 'https://example.com/v1', model: 'deepseek-v4-flash',
      credentials: { apiKey: 'deepseek-key' }, options: { thinkingMode: 'off', temperature: '0.4', maxTokens: '1024' } }) });
  assert.equal(deepseekProvider.status, 200);
  const deepseek = await fetch(`${base}/api/device/v1/llm`, { method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ messages: [{ role: 'user', content: '你好' }] }) });
  assert.equal(deepseek.status, 200);
  assert.equal(llmRequests[1].model, 'deepseek-v4-flash');
  assert.deepEqual(llmRequests[1].thinking, { type: 'disabled' });
  assert.equal(llmRequests[1].temperature, 0.4);
  assert.equal(llmRequests[1].max_tokens, 1024);

  const asrProvider = await fetch(`${base}/api/admin/providers`, { method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ kind: 'asr', provider: 'qwen', label: 'Ali Paraformer', enabled: true, model: 'paraformer-realtime-v2',
      credentials: { apiKey: 'ali-asr-key', workspaceId: 'workspace-1' }, options: { languageHints: ['zh', 'en'] } }) });
  assert.equal(asrProvider.status, 200);
  const audio = Buffer.alloc(3200, 1);
  const asr = await fetch(`${base}/api/device/v1/asr?format=pcm&sampleRate=16000`, { method: 'POST', headers: {
    ...deviceHeaders, 'content-type': 'audio/pcm',
  }, body: audio });
  assert.equal(asr.status, 200);
  assert.equal((await asr.json()).text, '测试成功');
  assert.equal(asrAuthorization, 'Bearer ali-asr-key');
  assert.equal(asrAudioBytes, audio.length);

  const ttsProvider = await fetch(`${base}/api/admin/providers`, { method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ kind: 'tts', provider: 'volcengine', label: 'Volcengine Seed TTS', enabled: true, model: 'seed-tts-2.0',
      credentials: { apiKey: 'volc-tts-key' }, options: { speaker: 'zh_female_vv_uranus_bigtts', audioFormat: 'mp3', sampleRate: '24000' } }) });
  assert.equal(ttsProvider.status, 200);
  const ttsCredentialUpdate = await fetch(`${base}/api/admin/providers`, { method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ kind: 'tts', provider: 'volcengine', label: 'Volcengine Seed TTS', enabled: true, model: 'seed-tts-2.0',
      credentials: { resourceId: 'seed-tts-2.0' }, options: { speaker: 'zh_female_vv_uranus_bigtts', audioFormat: 'mp3', sampleRate: '24000' } }) });
  assert.equal(ttsCredentialUpdate.status, 200);
  const tts = await fetch(`${base}/api/device/v1/tts`, { method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ text: '你好，小坦克' }) });
  assert.equal(tts.status, 200);
  assert.equal(tts.headers.get('content-type'), 'audio/mpeg');
  assert.equal(tts.headers.get('x-audio-sample-rate'), '24000');
  assert.equal(Buffer.from(await tts.arrayBuffer()).toString(), 'mock-audio');
  assert.equal(ttsApiKey, 'volc-tts-key', 'partial credential updates must preserve the existing API key');
  assert.equal(ttsRequest.req_params.speaker, 'zh_female_vv_uranus_bigtts');
  assert.equal(ttsRequest.req_params.audio_params.sample_rate, 24000);
  assert.equal(ttsRequest.req_params.sample_rate, undefined);

  const pcmProvider = await fetch(`${base}/api/admin/providers`, { method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ kind: 'tts', provider: 'volcengine', label: 'Volcengine Seed TTS', enabled: true, model: 'seed-tts-2.0',
      credentials: {}, options: { speaker: 'zh_female_vv_uranus_bigtts', audioFormat: 'pcm', sampleRate: '16000' } }) });
  assert.equal(pcmProvider.status, 200);
  const conversation = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&curiosity=61&warmth=72&talkativeness=35`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(conversation.status, 200);
  assert.equal(conversation.headers.get('x-audio-sample-rate'), '16000');
  assert.equal(conversation.headers.get('x-ai-emotion'), 'calm');
  assert.equal(conversation.headers.get('x-ai-mood'), 'calm');
  assert.equal(conversation.headers.get('x-ai-thought'), 'cloud');
  assert.match(conversation.headers.get('x-ai-mood-valence'), /^-?\d+$/);
  assert.equal(conversation.headers.get('x-ai-action-status'), 'none');
  assert.equal(conversation.headers.get('x-ai-action-mode'), 'shadow');
  assert.match(conversation.headers.get('server-timing'), /asr;dur=.*llm;dur=.*tts;dur=/);
  assert.equal(conversation.headers.get('x-turn-pcm-duration-ms'), '0');
  assert.match(conversation.headers.get('x-turn-trace-id'), /^[0-9a-f-]{36}$/);
  assert.equal(Buffer.from(await conversation.arrayBuffer()).toString(), 'mock-audio');
  const moodState = await fetch(`${base}/api/device/v1/mood`, {
    headers: deviceHeaders,
  });
  assert.equal(moodState.status, 200);
  const moodBody = await moodState.json();
  assert.equal(moodBody.version, 1);
  assert.equal(moodBody.thought.key, 'cloud');
  assert.equal(moodBody.thought.text, '小云朵慢慢飘过来啦');
  assert.equal(moodBody.thought.displayText, '软绵绵');
  const adminMood = await fetch(`${base}/api/admin/mood?deviceId=tank-001`, {
    headers: adminHeaders,
  });
  assert.equal(adminMood.status, 200);
  assert.equal((await adminMood.json()).events[0].source, 'speech');
  assert.match(llmRequests.at(-1).messages[1].content, /好奇心=61，亲近感=72，话语量=35/);
  assert.equal(llmRequests.at(-1).messages.at(-1).content, '测试成功');
  assert.equal(ttsRequest.req_params.audio_params.format, 'pcm');

  asrText = '你看看眼前是什么';
  llmReply = JSON.stringify({ reply: '我来看一眼。', emotion: 'curious',
    intensity: 65, action: null, camera: 'look' });
  const look = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(look.status, 200);
  assert.equal(look.headers.get('x-ai-action-type'), 'look');
  assert.equal(look.headers.get('x-ai-action-version'), '5');
  assert.equal((await look.arrayBuffer()).byteLength, 0);
  const lookTrace = look.headers.get('x-turn-trace-id');
  visionReply = JSON.stringify({ reply: '我看到一只小熊。', emotion: 'happy',
    intensity: 70, action: null, camera: null });
  const lookJpeg = Buffer.alloc(128, 0x11);
  lookJpeg[0] = 0xff; lookJpeg[1] = 0xd8;
  lookJpeg[126] = 0xff; lookJpeg[127] = 0xd9;
  const described = await fetch(`${base}/api/device/v1/vision/describe?traceId=${lookTrace}`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'image/jpeg' }, body: lookJpeg,
  });
  assert.equal(described.status, 200);
  assert.equal(described.headers.get('x-audio-format'), 'pcm');
  assert.equal(Buffer.from(await described.arrayBuffer()).toString(), 'mock-audio');
  assert.match(visionRequests.at(-1).messages.at(-1).content[0].image_url.url,
    /^data:image\/jpeg;base64,/);
  assert.match(visionRequests.at(-1).messages.at(-1).content[1].text,
    /请只回答孩子刚说的话：“你看看眼前是什么”/);
  assert.match(visionRequests.at(-1).messages[1].content, /普通聊天.*绝不能主动描述/);
  const replay = await fetch(`${base}/api/device/v1/vision/describe?traceId=${lookTrace}`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'image/jpeg' }, body: lookJpeg,
  });
  assert.equal(replay.status, 409, 'a photo authorization is single use');
  const cameraAudit = new DatabaseSync(config.dbPath);
  assert.deepEqual(cameraAudit.prepare('SELECT event FROM ai_camera_events WHERE trace_id=? ORDER BY id')
    .all(lookTrace).map((row) => row.event), ['requested', 'received', 'answered']);
  assert.equal(cameraAudit.prepare('SELECT COUNT(*) AS count FROM ai_action_execution WHERE trace_id=?')
    .get(lookTrace).count, 0, 'looking must not authorize motion');
  cameraAudit.close();
  asrText = '眼前是什么';
  visionReply = JSON.stringify({ reply: '可以一起玩猜谜！', emotion: 'happy',
    intensity: 70, action: { type: 'drive', direction: 'forward', durationMs: 2000 },
    camera: 'look' });
  const llmCountBeforeVisualTurn = llmRequests.length;
  const visualTurn = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1&vision=always`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(visualTurn.status, 200);
  assert.equal(visualTurn.headers.get('x-ai-action-type'), 'look');
  assert.equal(llmRequests.length, llmCountBeforeVisualTurn,
    'visual turns must not spend an extra text-model call before capture');
  const visualTrace = visualTurn.headers.get('x-turn-trace-id');
  const visualAnswer = await fetch(`${base}/api/device/v1/vision/describe?traceId=${visualTrace}`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'image/jpeg' }, body: lookJpeg,
  });
  assert.equal(visualAnswer.status, 200);
  assert.equal(Buffer.from(await visualAnswer.arrayBuffer()).toString(), 'mock-audio');
  assert.match(visionRequests.at(-1).messages.at(-1).content[0].image_url.url,
    /^data:image\/jpeg;base64,/);
  assert.match(visionRequests.at(-1).messages.at(-1).content[1].text,
    /请只回答孩子刚说的话：“眼前是什么”/);
  const visualAudit = new DatabaseSync(config.dbPath);
  assert.equal(visualAudit.prepare('SELECT COUNT(*) AS count FROM ai_action_execution WHERE trace_id=?')
    .get(visualTrace).count, 0, 'vision-model action fields must never move the chassis');
  visualAudit.close();
  asrText = '今天想玩什么';
  llmReply = JSON.stringify({ reply: '我们可以玩猜谜！', emotion: 'happy',
    intensity: 70, camera: null, action: null });
  const visualCountBeforeChat = visionRequests.length;
  const chatTurn = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1&vision=always`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(chatTurn.status, 200);
  assert.notEqual(chatTurn.headers.get('x-ai-action-type'), 'look',
    'ordinary chat must not take a photo by default');
  await chatTurn.arrayBuffer();
  assert.equal(visionRequests.length, visualCountBeforeChat);
  llmReply = JSON.stringify({ reply: '让我看一下。', emotion: 'curious',
    intensity: 70, camera: 'look', action: null });
  const modelLookTurn = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1&vision=always`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(modelLookTurn.status, 200);
  assert.equal(modelLookTurn.headers.get('x-ai-action-type'), 'look',
    'the model may request a frame when ASR rules did not match');
  const modelLookTrace = modelLookTurn.headers.get('x-turn-trace-id');
  await modelLookTurn.arrayBuffer();
  const modelLookAnswer = await fetch(`${base}/api/device/v1/vision/describe?traceId=${modelLookTrace}`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'image/jpeg' }, body: lookJpeg,
  });
  assert.equal(modelLookAnswer.status, 200);
  await modelLookAnswer.arrayBuffer();
  llmReply = '你好';
  asrText = '测试成功';
  visionReply = JSON.stringify({ move: 'left', durationMs: 3000,
    lightMode: 'rainbow', emotion: 'curious', intensity: 60,
    description: '左侧可以观察' });

  llmReply = JSON.stringify({ reply: '咦？真漂亮！', emotion: 'calm', intensity: 50,
    speechSegments: [
      { text: '咦？', emotion: 'curious', intensity: 60 },
      { text: '真漂亮！', emotion: 'happy', intensity: 80 },
    ], action: null });
  const expressive = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&diagnostic=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(expressive.status, 200);
  assert.equal(expressive.headers.get('x-ai-emotion'), 'curious');
  assert.equal(expressive.headers.get('x-ai-face-cues'), '0:curious:60,5:happy:80');
  assert.equal(Buffer.from(await expressive.arrayBuffer()).toString(), 'mock-audiomock-audio');
  llmReply = JSON.stringify({ reply: '咦？真漂亮！', emotion: 'calm', intensity: 50,
    action: null });
  const planned = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&diagnostic=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(planned.status, 200);
  assert.equal(planned.headers.get('x-ai-face-cues'), '0:calm:50');
  assert.equal(Buffer.from(await planned.arrayBuffer()).toString(), 'mock-audio');
  llmReply = '你好';

  asrText = '你休息吧';
  const sleepTurn = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&diagnostic=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(sleepTurn.status, 200);
  assert.equal(sleepTurn.headers.get('x-ai-session'), 'sleep');
  await sleepTurn.arrayBuffer();
  asrText = '测试成功';

  const beforeStreamBytes = asrAudioBytes;
  let forwardedBeforeUploadFinished = false;
  async function* liveMic() {
    yield Buffer.alloc(3_200, 1);
    await new Promise((resolve) => setTimeout(resolve, 50));
    forwardedBeforeUploadFinished = asrAudioBytes > beforeStreamBytes;
    yield Buffer.alloc(3_200, 2);
  }
  const liveConversation = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' },
    body: Readable.from(liveMic()), duplex: 'half',
  });
  assert.equal(liveConversation.status, 200);
  assert.equal(Buffer.from(await liveConversation.arrayBuffer()).toString(), 'mock-audio');
  assert.equal(forwardedBeforeUploadFinished, true, 'ASR should receive PCM while microphone upload is in progress');
  const streamingMetricsDb = new DatabaseSync(config.dbPath);
  const streamingMetrics = streamingMetricsDb.prepare('SELECT metrics_json FROM conversation_metrics ORDER BY id DESC LIMIT 1').get();
  streamingMetricsDb.close();
  assert.equal(JSON.parse(streamingMetrics.metrics_json).streaming, true);
  assert.equal(JSON.parse(streamingMetrics.metrics_json).inputBytes, 6_400);

  holdTtsSecondChunk = true;
  const framedConversation = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1&diagnostic=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' },
    body: Buffer.alloc(6_400),
  });
  assert.equal(framedConversation.status, 200);
  assert.equal(framedConversation.headers.get('x-babytank-audio-framing'), 'v1');
  const framedReader = framedConversation.body.getReader();
  const firstFrame = Buffer.from((await framedReader.read()).value);
  assert.equal(firstFrame[0], 0x50);
  assert.equal(firstFrame.readUInt32BE(1), 5);
  assert.equal(firstFrame.subarray(5).toString(), 'mock-');
  assert.equal(typeof releaseTtsSecondChunk, 'function',
    'first audio must be delivered before upstream TTS has completed');
  releaseTtsSecondChunk();
  holdTtsSecondChunk = false;
  const restFrames = [];
  for (;;) {
    const part = await framedReader.read();
    if (part.done) break;
    restFrames.push(Buffer.from(part.value));
  }
  const framedBody = Buffer.concat([firstFrame, ...restFrames]);
  assert.equal(framedBody[0], 0x50);
  assert.equal(framedBody.subarray(5, 10).toString(), 'mock-');
  assert.equal(framedBody[10], 0x50);
  assert.equal(framedBody.subarray(15).toString(), 'audio');

  llmReply = JSON.stringify({ reply: '咦？真漂亮！', emotion: 'curious', intensity: 60,
    speechSegments: [{ text: '咦？', emotion: 'curious', intensity: 60 },
      { text: '真漂亮！', emotion: 'happy', intensity: 80 }], action: null });
  const framedEmotions = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1&diagnostic=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' },
    body: Buffer.alloc(6_400),
  });
  assert.equal(framedEmotions.status, 200);
  assert.equal(framedEmotions.headers.get('x-ai-face-cues'), '0:curious:60');
  const framedEmotionBody = Buffer.from(await framedEmotions.arrayBuffer());
  const frameTypes = [];
  let framedOffset = 0;
  while (framedOffset < framedEmotionBody.length) {
    const type = framedEmotionBody[framedOffset];
    const length = framedEmotionBody.readUInt32BE(framedOffset + 1);
    frameTypes.push(type);
    if (type === 0x45) {
      assert.equal(framedEmotionBody.subarray(framedOffset + 5,
        framedOffset + 5 + length).toString(), 'happy:80');
    }
    framedOffset += 5 + length;
  }
  assert.deepEqual(frameTypes, [0x50, 0x50, 0x45, 0x50, 0x50]);
  llmReply = '你好';

  const touchConversation = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&touch=left`, {
    method: 'POST', headers: deviceHeaders,
  });
  assert.equal(touchConversation.status, 200);
  await touchConversation.arrayBuffer();
  assert.match(llmRequests.at(-1).messages.at(-1).content, /左侧轻轻摸了你一下/);
  assert.equal(llmRequests.at(-1).messages.filter((item) => item.role === 'assistant').length, 6);

  asrText = '画个小汽车';
  const curatedDrawing = { durationMs: 6000, items: [
    { emoji: '🚗', x: 120, y: 120, size: 188, rotation: 0, flipX: false },
  ] };
  drawingProposalReply = JSON.stringify({ drawing: curatedDrawing });
  llmReply = JSON.stringify({ reply: '给你画一辆小汽车。', emotion: 'happy',
    intensity: 70, action: null, camera: null, drawing: null });
  const drawReply = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(drawReply.status, 200);
  assert.equal(drawReply.headers.get('x-ai-action-type'), 'draw');
  assert.equal(drawReply.headers.get('x-ai-action-version'), '6');
  const drawTrace = drawReply.headers.get('x-turn-trace-id');
  await drawReply.arrayBuffer();
  const drawing = await fetch(`${base}/api/device/v1/drawing?traceId=${drawTrace}`, {
    headers: deviceHeaders,
  });
  assert.equal(drawing.status, 200);
  assert.equal(drawing.headers.get('content-type'), 'image/jpeg');
  assert.equal(drawing.headers.get('x-drawing-duration-ms'), '6000');
  const drawingJpeg = Buffer.from(await drawing.arrayBuffer());
  assert.deepEqual([...drawingJpeg.subarray(0, 2)], [0xff, 0xd8]);
  const drawingReplay = await fetch(`${base}/api/device/v1/drawing?traceId=${drawTrace}`,
    { headers: deviceHeaders });
  assert.equal(drawingReplay.status, 409);
  const drawingAudit = new DatabaseSync(config.dbPath);
  assert.equal(drawingAudit.prepare('SELECT COUNT(*) AS count FROM ai_action_execution WHERE trace_id=?')
    .get(drawTrace).count, 0, 'drawing must not authorize motion');
  drawingAudit.close();
  drawingProposalReply = JSON.stringify({ drawing: curatedDrawing });
  llmReply = JSON.stringify({ reply: '给你画自行车。', emotion: 'happy',
    intensity: 70, action: null, drawing: null });
  asrText = '画辆自行车';
  const focusedDrawing = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(focusedDrawing.status, 200);
  assert.equal(focusedDrawing.headers.get('x-ai-action-type'), 'draw');
  const focusedTrace = focusedDrawing.headers.get('x-turn-trace-id');
  await focusedDrawing.arrayBuffer();
  const focusedScene = await fetch(`${base}/api/device/v1/drawing?traceId=${focusedTrace}`,
    { headers: deviceHeaders });
  assert.equal(focusedScene.headers.get('content-type'), 'image/jpeg');
  assert.ok((await focusedScene.arrayBuffer()).byteLength > 256);
  drawingProposalReply = null;
  asrText = '测试成功';
  llmReply = '你好';

  const diagnostic = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&diagnostic=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(diagnostic.status, 200);
  await diagnostic.arrayBuffer();
  assert.equal(llmRequests.at(-1).messages.filter((item) => item.role === 'assistant').length, 0);
  const metricsDb = new DatabaseSync(config.dbPath);
  const savedMetrics = metricsDb.prepare('SELECT metrics_json FROM conversation_metrics ORDER BY id DESC LIMIT 1').get();
  metricsDb.close();
  assert.equal(JSON.parse(savedMetrics.metrics_json).diagnostic, true);
  assert.equal(savedMetrics.metrics_json.includes('测试成功'), false, 'metrics must not retain recognized speech');

  asrText = '向前走一步';
  llmReply = JSON.stringify({ reply: '好，我向前挪一点', emotion: 'happy', intensity: 60,
    action: { type: 'drive', direction: 'forward', durationMs: 3000 } });
  const driveProposal = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&diagnostic=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(driveProposal.status, 200);
  assert.equal(driveProposal.headers.get('x-ai-action-mode'), 'shadow');
  assert.equal(driveProposal.headers.get('x-ai-action-status'), 'shadow');
  assert.equal(driveProposal.headers.get('x-ai-action-type'), 'drive');
  assert.equal(driveProposal.headers.get('x-ai-action-direction'), null);
  assert.equal(driveProposal.headers.get('x-ai-action-speed-per-mille'), null);
  assert.equal(driveProposal.headers.get('x-ai-action-duration-ms'), null);
  assert.equal(Buffer.from(await driveProposal.arrayBuffer()).toString(), 'mock-audio');
  const unauthenticatedActions = await fetch(`${base}/api/admin/ai-actions`);
  assert.equal(unauthenticatedActions.status, 401);
  const actionList = await fetch(`${base}/api/admin/ai-actions?limit=10`, { headers: { cookie } });
  assert.equal(actionList.status, 200);
  const loggedDrive = (await actionList.json()).actions[0];
  assert.equal(loggedDrive.trace_id, driveProposal.headers.get('x-turn-trace-id'));
  assert.equal(loggedDrive.action_type, 'drive');
  assert.equal(loggedDrive.direction, 'forward');
  assert.equal(loggedDrive.status, 'shadow');
  assert.equal(loggedDrive.proposal_status, 'shadow');
  assert.equal(loggedDrive.diagnostic, 1);
  assert.equal(loggedDrive.executed, 0, 'diagnostic AI drive proposals must never move the chassis');

  const driveApproved = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1&vision=always`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(driveApproved.status, 200);
  assert.equal(driveApproved.headers.get('x-ai-action-mode'), 'execute');
  assert.equal(driveApproved.headers.get('x-ai-action-status'), 'approved');
  assert.equal(driveApproved.headers.get('x-ai-action-type'), 'drive',
    'explicit motor commands must not wait for a photo');
  assert.equal(driveApproved.headers.get('x-ai-action-direction'), 'forward');
  assert.equal(driveApproved.headers.get('x-ai-action-speed-per-mille'), '900');
  assert.equal(driveApproved.headers.get('x-ai-action-duration-ms'), '3000');
  assert.equal(driveApproved.headers.get('x-ai-action-light-mode'), 'unchanged');
  await driveApproved.arrayBuffer();
  const traceId = driveApproved.headers.get('x-turn-trace-id');
  const actionReport = await fetch(`${base}/api/device/v1/ai-action/result`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ traceId, direction: 'forward', result: 'completed' }),
  });
  assert.equal(actionReport.status, 200);
  const executedList = await fetch(`${base}/api/admin/ai-actions?limit=1`, { headers: { cookie } });
  const executedAction = (await executedList.json()).actions[0];
  assert.equal(executedAction.executed, 1);
  assert.equal(executedAction.execution_result, 'completed');
  assert.equal(executedAction.dispatch_mode, 'execute');
  assert.equal(executedAction.status, 'approved');
  assert.equal(executedAction.proposal_status, 'shadow');
  asrText = '前进3秒';
  llmReply = JSON.stringify({ reply: '好，前进三秒', action: { type: 'drive', direction: 'forward', durationMs: 3000 } });
  const timedDrive = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(timedDrive.headers.get('x-ai-action-mode'), 'execute');
  assert.equal(timedDrive.headers.get('x-ai-action-duration-ms'), '3000');
  await timedDrive.arrayBuffer();
  asrText = '向前走一步';
  llmReply = JSON.stringify({ reply: '好，我来了', emotion: 'excited', intensity: 72,
    action: null });
  const deterministicDrive = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(deterministicDrive.headers.get('x-ai-action-mode'), 'execute');
  assert.equal(deterministicDrive.headers.get('x-ai-action-direction'), 'forward');
  assert.equal(deterministicDrive.headers.get('x-ai-action-duration-ms'), '2000');
  await deterministicDrive.arrayBuffer();
  asrText = '为什么不能前进';
  llmReply = JSON.stringify({ reply: '我们先聊聊', action: {
    type: 'drive', direction: 'forward', durationMs: 3000, lightMode: 'rainbow' } });
  const indirect = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(indirect.headers.get('x-ai-action-mode'), 'execute');
  assert.equal(indirect.headers.get('x-ai-action-status'), 'approved');
  assert.equal(indirect.headers.get('x-ai-action-direction'), 'forward');
  await indirect.arrayBuffer();

  llmReply = JSON.stringify({ reply: '先不动', action: { type: 'drive', direction: 'forward', speed: 1000 } });
  const invalidDrive = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&diagnostic=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(invalidDrive.status, 200);
  assert.equal(invalidDrive.headers.get('x-ai-action-status'), 'rejected');
  assert.equal(invalidDrive.headers.get('x-ai-action-type'), 'none');
  await invalidDrive.arrayBuffer();
  const actionListAfterReject = await fetch(`${base}/api/admin/ai-actions?limit=1`, { headers: { cookie } });
  const loggedReject = (await actionListAfterReject.json()).actions[0];
  assert.equal(loggedReject.reason, 'action_schema_invalid');
  assert.equal(loggedReject.executed, 0);

  asrText = '去看看前面有什么';
  llmReply = JSON.stringify({ reply: '我先看看。', emotion: 'curious', intensity: 60,
    action: null });
  const explore = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(explore.status, 200);
  assert.equal(explore.headers.get('x-ai-action-version'), '3');
  assert.equal(explore.headers.get('x-ai-action-type'), 'explore');
  assert.equal(explore.headers.get('x-ai-action-status'), 'approved');
  const exploreTrace = explore.headers.get('x-turn-trace-id');
  await explore.arrayBuffer();
  const jpeg = Buffer.alloc(1024, 0x55);
  jpeg[0] = 0xff; jpeg[1] = 0xd8; jpeg[jpeg.length - 2] = 0xff; jpeg[jpeg.length - 1] = 0xd9;
  const observeUrl = `${base}/api/device/v1/explore/observe?traceId=${exploreTrace}&step=0`;
  const wrongImage = await fetch(observeUrl, { method: 'POST',
    headers: { ...deviceHeaders, 'content-type': 'image/jpeg' }, body: Buffer.alloc(1024) });
  assert.equal(wrongImage.status, 400);
  const observation = await fetch(observeUrl, { method: 'POST',
    headers: { ...deviceHeaders, 'content-type': 'image/jpeg' }, body: jpeg });
  assert.equal(observation.status, 200);
  const decision = await observation.json();
  assert.equal(decision.move, 'left');
  assert.equal(decision.durationMs, 3000);
  assert.equal(visionRequests.at(-1).model, 'qwen3-vl-plus');
  assert.match(visionRequests.at(-1).messages[1].content[1].image_url.url,
    /^data:image\/jpeg;base64,/);
  const duplicate = await fetch(observeUrl, { method: 'POST',
    headers: { ...deviceHeaders, 'content-type': 'image/jpeg' }, body: jpeg });
  assert.equal(duplicate.status, 409);
  const stepReport = await fetch(`${base}/api/device/v1/explore/result`, { method: 'POST',
    headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ traceId: exploreTrace, step: 0, direction: 'left',
      result: 'completed', reason: 'stop_echo_ok' }) });
  assert.deepEqual(await stepReport.json(), { ok: true, finished: false,
    usedSteps: 1, usedMs: 3000 });
  visionReply = JSON.stringify({ move: 'stop', durationMs: 0,
    lightMode: 'sleep', emotion: 'calm', intensity: 55,
    description: '已经看清楚了' });
  const stopped = await fetch(`${base}/api/device/v1/explore/observe?traceId=${exploreTrace}&step=1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'image/jpeg' }, body: jpeg });
  assert.equal((await stopped.json()).move, 'stop');
  const explorationEvents = await fetch(`${base}/api/admin/ai-explorations`, { headers: { cookie } });
  assert.equal(explorationEvents.status, 200);
  assert.deepEqual((await explorationEvents.json()).events.map((event) => event.event).slice(0, 5),
    ['stopped', 'observed', 'completed', 'planned', 'observed']);

  llmReply = JSON.stringify({ reply: '摸摸你', action: { type: 'drive', direction: 'forward', durationMs: 3000 } });
  const touchCannotDrive = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000&touch=left&diagnostic=1`, {
    method: 'POST', headers: deviceHeaders,
  });
  assert.equal(touchCannotDrive.status, 200);
  assert.equal(touchCannotDrive.headers.get('x-ai-action-status'), 'rejected');
  await touchCannotDrive.arrayBuffer();
  llmReply = '你好';

  const image = Buffer.alloc(1024, 0x5a);
  image[0] = 0xe9;
  const upload = await fetch(`${base}/api/admin/firmware?version=1.2.3&board=baby-tank-s3&channel=stable&fileName=firmware.bin&chip=esp32s3&secureVersion=2&rolloutPercent=100&secureBootRequired=true`, {
    method: 'PUT', headers: { ...adminHeaders, 'content-type': 'application/octet-stream' }, body: image,
  });
  assert.equal(upload.status, 201);
  const release = await upload.json();
  const publish = await fetch(`${base}/api/admin/firmware/${release.id}/publish`, { method: 'POST', headers: adminHeaders, body: '{}' });
  assert.equal(publish.status, 200);

  const noSecureBoot = await fetch(`${base}/api/device/v1/ota/check?version=1.0.0&chip=esp32s3&secureVersion=1`, { headers: deviceHeaders });
  assert.deepEqual(await noSecureBoot.json(), { updateAvailable: false });
  const check = await fetch(`${base}/api/device/v1/ota/check?version=1.0.0&chip=esp32s3&secureVersion=1&secureBoot=1`, { headers: deviceHeaders });
  const available = await check.json();
  assert.equal(available.updateAvailable, true);
  assert.equal(available.secureVersion, 2);

  const basic = Buffer.from(`tank-001:${token}`).toString('base64');
  const download = await fetch(available.url, { headers: { authorization: `Basic ${basic}`, range: 'bytes=0-15' } });
  assert.equal(download.status, 206);
  assert.equal((await download.arrayBuffer()).byteLength, 16);

  const report = await fetch(`${base}/api/device/v1/ota/report`, { method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ event: 'validated', version: '1.2.3' }) });
  assert.equal(report.status, 200);

  const newPassword = 'a-new-strong-password-1234';
  const changePassword = await fetch(`${base}/api/auth/change-password`, { method: 'POST', headers: adminHeaders,
    body: JSON.stringify({ currentPassword: config.adminPassword, newPassword }) });
  assert.equal(changePassword.status, 200);
  assert.equal((await changePassword.json()).reauthenticationRequired, true);

  const expiredSession = await fetch(`${base}/api/admin/overview`, { headers: { cookie } });
  assert.equal(expiredSession.status, 401);
  const oldPasswordLogin = await fetch(`${base}/api/auth/login`, { method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ username: 'admin', password: config.adminPassword }) });
  assert.equal(oldPasswordLogin.status, 401);
  const newPasswordLogin = await fetch(`${base}/api/auth/login`, { method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ username: 'admin', password: newPassword }) });
  assert.equal(newPasswordLogin.status, 200);

  asrText = '今天想玩什么';
  llmReply = JSON.stringify({ reply: '我们玩猜谜。', emotion: 'happy', intensity: 70,
    camera: null, action: null });
  const voiceSocket = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/v1/conversation/ws`,
    { headers: deviceHeaders });
  await once(voiceSocket, 'open');
  const asrBytesBeforeSocket = asrAudioBytes;
  async function websocketVoiceTurn() {
    const received = [];
    const finished = new Promise((resolve, reject) => {
      const onMessage = (payload, binary) => {
        if (binary) {
          received.push(Buffer.from(payload));
          return;
        }
        const message = JSON.parse(String(payload));
        if (message.type === 'turn_error') {
          voiceSocket.off('message', onMessage);
          reject(new Error(`${message.status}: ${message.code}`));
        } else if (message.type === 'reply') {
          assert.equal(message.status, 200);
          assert.equal(message.headers['x-babytank-audio-framing'], 'v1');
        } else if (message.type === 'turn_end') {
          voiceSocket.off('message', onMessage);
          resolve(Buffer.concat(received));
        }
      };
      voiceSocket.on('message', onMessage);
    });
    voiceSocket.send(JSON.stringify({ type: 'listen', state: 'start',
      curiosity: 78, warmth: 84, talkativeness: 46 }));
    voiceSocket.send(audio, { binary: true });
    voiceSocket.send(JSON.stringify({ type: 'listen', state: 'stop' }));
    return finished;
  }
  const firstSocketTurn = await websocketVoiceTurn();
  const secondSocketTurn = await websocketVoiceTurn();
  assert.equal(firstSocketTurn[0], 0x50);
  assert.equal(secondSocketTurn[0], 0x50);
  assert.equal(asrAudioBytes - asrBytesBeforeSocket, audio.length * 2,
    'two turns must share one authenticated WebSocket without losing microphone frames');
  voiceSocket.close();
  await once(voiceSocket, 'close');
});
