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
import sharp from 'sharp';
import { createApp } from '../src/server.mjs';

test('admin, provider, device and safe OTA flow', async (t) => {
  const nativeFetch = globalThis.fetch;
  let upstreamAuthorization = '';
  const llmRequests = [];
  let llmReply = '你好';
  let driveRouterReply = null;
  let drawingProposalReply = null;
  let drawingProposalReplies = null;
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
      const content = request.messages[0]?.content.includes('行走工具选择器')
        ? driveRouterReply ?? JSON.stringify({ action: (() => {
          try { return JSON.parse(llmReply).action ?? null; } catch { return null; }
        })() })
        : request.messages[0]?.content.includes('儿童矢量插画的画师')
          ? drawingProposalReplies?.shift() ?? drawingProposalReply ?? llmReply : llmReply;
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

  const homePage = await fetch(`${base}/`);
  assert.equal(homePage.status, 200);
  assert.match(await homePage.text(), /会看你/);

  const consolePage = await fetch(`${base}/console`);
  assert.equal(consolePage.status, 200);
  assert.match(await consolePage.text(), /创建云端账号/);

  for (const route of ['/account', '/account/']) {
    const accountPage = await fetch(`${base}${route}`);
    assert.equal(accountPage.status, 200);
    assert.match(await accountPage.text(), /修改管理员密码/);
  }

  const noPublicRelease = await fetch(`${base}/api/public/v1/releases`);
  assert.deepEqual(await noPublicRelease.json(), { releases: [] });

  const missingConsent = await fetch(`${base}/api/cloud/v1/register`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ email: 'guardian@example.com', displayName: '小坦克家长',
      password: 'cloud-password-1234' }),
  });
  assert.equal(missingConsent.status, 400);
  const registration = await fetch(`${base}/api/cloud/v1/register`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ email: 'guardian@example.com', displayName: '小坦克家长',
      password: 'cloud-password-1234', guardianConfirmed: true, termsAccepted: true }),
  });
  assert.equal(registration.status, 201);
  const duplicateRegistration = await fetch(`${base}/api/cloud/v1/register`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ email: 'GUARDIAN@example.com', displayName: '重复用户',
      password: 'cloud-password-5678', guardianConfirmed: true, termsAccepted: true }),
  });
  assert.equal(duplicateRegistration.status, 409);

  const cloudLogin = await fetch(`${base}/api/cloud/v1/login`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ email: 'guardian@example.com', password: 'cloud-password-1234' }),
  });
  assert.equal(cloudLogin.status, 200);
  const cloudAuth = await cloudLogin.json();
  const cloudCookie = cloudLogin.headers.get('set-cookie').split(';')[0];
  const cloudMe = await fetch(`${base}/api/cloud/v1/me`, { headers: { cookie: cloudCookie } });
  assert.equal((await cloudMe.json()).account.email, 'guardian@example.com');
  const cloudLogout = await fetch(`${base}/api/cloud/v1/logout`, { method: 'POST',
    headers: { cookie: cloudCookie, 'x-csrf-token': cloudAuth.csrfToken } });
  assert.equal(cloudLogout.status, 200);

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
  const identity = await fetch(`${base}/api/device/v1/identity`,
    { headers: deviceHeaders });
  assert.equal(identity.status, 200);
  assert.equal((await identity.json()).deviceId, 'tank-001');
  const anonymousIdentity = await fetch(`${base}/api/device/v1/identity`);
  assert.equal(anonymousIdentity.status, 401);
  const pairing = await fetch(`${base}/api/admin/parent-claims`, {
    method: 'POST', headers: adminHeaders,
    body: JSON.stringify({ deviceId: 'tank-001' }) });
  assert.equal(pairing.status, 201);
  const pairingCode = (await pairing.json()).code;
  const parentClaim = await fetch(`${base}/api/parent/v1/claim`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ code: pairingCode }) });
  assert.equal(parentClaim.status, 200);
  const parentToken = (await parentClaim.json()).token;
  assert.notEqual(parentToken, token);
  const parentHeaders = { authorization: `Bearer ${parentToken}` };
  const replayClaim = await fetch(`${base}/api/parent/v1/claim`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ code: pairingCode }) });
  assert.equal(replayClaim.status, 401);
  const parentMe = await fetch(`${base}/api/parent/v1/me`, { headers: parentHeaders });
  assert.equal((await parentMe.json()).deviceId, 'tank-001');
  const secondEnrollment = await fetch(`${base}/api/admin/devices`, {
    method: 'POST', headers: adminHeaders,
    body: JSON.stringify({ deviceId: 'tank-002', name: 'Other Tank', board: 'baby-tank-s3' }) });
  assert.equal(secondEnrollment.status, 201);
  const scopedState = await fetch(`${base}/api/parent/v1/state?deviceId=tank-002`,
    { headers: parentHeaders });
  assert.equal((await scopedState.json()).deviceId, 'tank-001',
    'a query parameter cannot switch the parent session to another device');
  const privateWithoutParent = await fetch(`${base}/api/parent/v1/questions`,
    { headers: deviceHeaders });
  assert.equal(privateWithoutParent.status, 401);

  const stateBefore = await fetch(`${base}/api/device/v2/state`, { headers: deviceHeaders });
  assert.equal(stateBefore.status, 200);
  const stateBeforeBody = await stateBefore.json();
  assert.equal(stateBeforeBody.state.revision, 1);
  assert.equal(stateBeforeBody.state.enforcement, 'preview_only');
  const stateEventId = 'cf2c0ec5-78ba-426a-9046-71aa1d18e75e';
  const stateEvent = await fetch(`${base}/api/device/v2/state/events`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ eventId: stateEventId, kind: 'side_touch' }) });
  assert.equal(stateEvent.status, 200);
  assert.equal((await stateEvent.json()).state.needs.bond, 57);
  const stateRetry = await fetch(`${base}/api/device/v2/state/events`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ eventId: stateEventId, kind: 'side_touch' }) });
  assert.equal((await stateRetry.json()).duplicate, true);
  const policy = await fetch(`${base}/api/admin/character-state`, {
    method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ deviceId: 'tank-001', revision: 1,
      timeZone: 'Asia/Shanghai', deepNightLock: true }) });
  assert.equal(policy.status, 200);
  assert.equal((await policy.json()).state.policy.deepNightLock, true);
  const parentInteraction = await fetch(`${base}/api/parent/v1/events`, {
    method: 'POST', headers: { ...parentHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ eventId: '26328cb2-463b-44b3-95f7-0963893d808d', kind: 'drank',
      deviceId: 'tank-002' }) });
  assert.equal(parentInteraction.status, 200);
  assert.equal((await parentInteraction.json()).state.needs.thirst, 0);
  const otherState = await fetch(`${base}/api/admin/character-state?deviceId=tank-002`,
    { headers: adminHeaders });
  assert.equal((await otherState.json()).state.needs.thirst, 20,
    'parent event must update only its paired device');
  const memoryCreated = await fetch(`${base}/api/admin/approved-memories`, {
    method: 'POST', headers: adminHeaders,
    body: JSON.stringify({ deviceId: 'tank-001', kind: 'preference',
      subject: 'story_theme', value: '海洋动物' }) });
  assert.equal(memoryCreated.status, 201);
  const unauthenticatedMemory = await fetch(`${base}/api/admin/approved-memories?deviceId=tank-001`,
    { headers: deviceHeaders });
  assert.equal(unauthenticatedMemory.status, 401);
  const noCsrfMemory = await fetch(`${base}/api/admin/approved-memories`, {
    method: 'POST', headers: { cookie, 'content-type': 'application/json' },
    body: JSON.stringify({ deviceId: 'tank-001', kind: 'preference',
      subject: 'play_theme', value: '搭积木' }) });
  assert.equal(noCsrfMemory.status, 403);
  const approvedMemory = (await memoryCreated.json()).memory;
  const memoryListed = await fetch(`${base}/api/admin/approved-memories?deviceId=tank-001`,
    { headers: adminHeaders });
  assert.equal((await memoryListed.json()).memories[0].value, '海洋动物');
  const parentMemory = await fetch(`${base}/api/parent/v1/memories`,
    { headers: parentHeaders });
  assert.equal((await parentMemory.json()).memories[0].value, '海洋动物');
  const secondDeviceMemory = await fetch(`${base}/api/admin/approved-memories?deviceId=tank-002`,
    { headers: adminHeaders });
  assert.deepEqual((await secondDeviceMemory.json()).memories, []);
  const memoryDelete = await fetch(`${base}/api/admin/approved-memories?deviceId=tank-001&id=${approvedMemory.id}`,
    { method: 'DELETE', headers: adminHeaders });
  assert.equal((await memoryDelete.json()).removed, true);
  const archivePolicy = await fetch(`${base}/api/admin/question-history`, {
    method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ deviceId: 'tank-001', enabled: true, retentionDays: 7 }) });
  assert.equal(archivePolicy.status, 200);
  assert.equal((await archivePolicy.json()).policy.enabled, true);

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
  const typedId = 'dd617c68-093b-4b63-9a5b-a44fbd5f73fb';
  const typedBody = JSON.stringify({ requestId: typedId, text: '今天玩什么？' });
  const boundedWaitStartedAt = Date.now();
  const boundedWait = await fetch(
    `${base}/api/device/v1/text-turns/next?waitMs=100`,
    { headers: deviceHeaders });
  const boundedWaitElapsed = Date.now() - boundedWaitStartedAt;
  assert.equal(boundedWait.status, 204);
  assert.ok(boundedWaitElapsed >= 75 && boundedWaitElapsed < 1000,
    `bounded text wait should return promptly, elapsed=${boundedWaitElapsed}ms`);
  const typedSubmit = await fetch(`${base}/api/parent/v1/text-turns`, {
    method: 'POST', headers: { ...parentHeaders, 'content-type': 'application/json' },
    body: typedBody,
  });
  assert.equal(typedSubmit.status, 202,
    'a paused camera-mode poll must not be mistaken for an offline robot');
  llmReply = JSON.stringify({ reply: '我们来玩猜谜吧！', emotion: 'happy',
    intensity: 70, action: null });
  const typedPoll = await fetch(`${base}/api/device/v1/text-turns/next?wait=0`,
    { headers: deviceHeaders });
  assert.equal((await typedPoll.json()).id, typedId);
  const typedFollowupId = 'dd617c68-093b-4b63-9a5b-a44fbd5f73fa';
  const typedFollowup = await fetch(`${base}/api/parent/v1/text-turns`, {
    method: 'POST', headers: { ...parentHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ requestId: typedFollowupId, text: '再说一句' }),
  });
  assert.equal(typedFollowup.status, 202,
    'an active turn must queue, not reject, a new App or sketch request');
  const prematureFollowupClaim = await fetch(
    `${base}/api/device/v1/text-turns/next?wait=0`, { headers: deviceHeaders });
  assert.equal(prematureFollowupClaim.status, 204,
    'the device must still execute queued turns serially');
  const typedDuplicate = await fetch(`${base}/api/parent/v1/text-turns`, {
    method: 'POST', headers: { ...parentHeaders, 'content-type': 'application/json' },
    body: typedBody,
  });
  assert.equal(typedDuplicate.status, 200, 'retries must not replay an AI turn');
  const asrBeforeTyped = asrAudioBytes;
  const typedConversation = await fetch(`${base}/api/device/v1/conversation/text?jobId=${typedId}&wire=framed-v1&vision=always`, {
    method: 'POST', headers: deviceHeaders,
  });
  assert.equal(typedConversation.status, 200);
  assert.equal(typedConversation.headers.get('x-ai-action-type'), 'none');
  await typedConversation.arrayBuffer();
  assert.equal(asrAudioBytes, asrBeforeTyped, 'text turn must skip microphone and ASR');
  const typedMetricsDb = new DatabaseSync(config.dbPath);
  const typedMetricsRow = typedMetricsDb.prepare(
    'SELECT metrics_json FROM conversation_metrics ORDER BY id DESC LIMIT 1').get();
  typedMetricsDb.close();
  const typedMetrics = JSON.parse(typedMetricsRow.metrics_json);
  assert.equal(typedMetrics.kind, 'text');
  assert.ok(typedMetrics.textQueueMs >= 0);
  assert.ok(typedMetrics.textClaimToRequestMs >= 0);
  const typedReply = await fetch(`${base}/api/parent/v1/text-turns/${typedId}`,
    { headers: parentHeaders });
  assert.equal((await typedReply.json()).reply, '我们来玩猜谜吧！');
  const typedAfterReplyId = 'dd617c68-093b-4b63-9a5b-a44fbd5f73f9';
  const typedAfterReply = await fetch(`${base}/api/parent/v1/text-turns`, {
    method: 'POST', headers: { ...parentHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ requestId: typedAfterReplyId, text: '还要再说一句' }),
  });
  assert.equal(typedAfterReply.status, 202,
    'reply_ready must not block a new App or sketch request when its completion ack is lost');
  const typedDone = await fetch(`${base}/api/device/v1/text-turns/${typedId}/complete`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ status: 'completed' }),
  });
  assert.equal(typedDone.status, 200);
  assert.equal((await (await fetch(`${base}/api/parent/v1/text-turns/${typedId}`,
    { headers: parentHeaders })).json()).status, 'completed');
  const typedFollowupClaim = await fetch(
    `${base}/api/device/v1/text-turns/next?wait=0`, { headers: deviceHeaders });
  assert.equal((await typedFollowupClaim.json()).id, typedFollowupId);
  const typedFollowupDone = await fetch(
    `${base}/api/device/v1/text-turns/${typedFollowupId}/complete`, {
      method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
      body: JSON.stringify({ status: 'blocked' }),
    });
  assert.equal(typedFollowupDone.status, 200);
  const typedAfterReplyClaim = await fetch(
    `${base}/api/device/v1/text-turns/next?wait=0`, { headers: deviceHeaders });
  assert.equal((await typedAfterReplyClaim.json()).id, typedAfterReplyId,
    'queued turns must preserve FIFO order');
  const typedAfterReplyDone = await fetch(
    `${base}/api/device/v1/text-turns/${typedAfterReplyId}/complete`, {
      method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
      body: JSON.stringify({ status: 'blocked' }),
    });
  assert.equal(typedAfterReplyDone.status, 200);

  const sketchId = 'f3555ed4-b076-4863-90e3-36f3873acf0d';
  const sketchJpeg = await sharp({
    create: { width: 384, height: 384, channels: 3, background: '#fffdf7' },
  }).composite([{
    input: Buffer.from('<svg width="384" height="384"><path d="M70 300 L192 70 L314 300 Z" fill="none" stroke="#17242d" stroke-width="22" stroke-linejoin="round"/></svg>'),
    top: 0, left: 0,
  }]).jpeg({ quality: 88 }).toBuffer();
  visionReply = JSON.stringify({ guess: '小飞机' });
  const llmBeforeSketch = llmRequests.length;
  const sketchSubmit = await fetch(`${base}/api/parent/v1/sketch-guesses`, {
    method: 'POST', headers: { ...parentHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ requestId: sketchId, round: 1,
      jpegBase64: sketchJpeg.toString('base64') }),
  });
  assert.equal(sketchSubmit.status, 202);
  const sketchQueued = await sketchSubmit.json();
  assert.equal(sketchQueued.guess, '小飞机');
  assert.equal(sketchQueued.reply, '我猜这是小飞机！');
  const sketchPoll = await fetch(`${base}/api/device/v1/text-turns/next?wait=0`,
    { headers: deviceHeaders });
  assert.equal((await sketchPoll.json()).id, sketchId);
  const sketchConversation = await fetch(
    `${base}/api/device/v1/conversation/text?jobId=${sketchId}&wire=framed-v1`, {
      method: 'POST', headers: deviceHeaders,
    });
  assert.equal(sketchConversation.status, 200);
  assert.equal(sketchConversation.headers.get('x-ai-action-type'), 'none');
  assert.equal(Buffer.from(await sketchConversation.arrayBuffer())[0], 0x50);
  assert.equal(ttsRequest.req_params.text, '我猜这是小飞机！');
  assert.equal(llmRequests.length, llmBeforeSketch,
    'guess playback must not trigger a second language-model request');
  const sketchDone = await fetch(`${base}/api/device/v1/text-turns/${sketchId}/complete`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ status: 'completed' }),
  });
  assert.equal(sketchDone.status, 200);
  const sketchFinal = await (await fetch(`${base}/api/parent/v1/text-turns/${sketchId}`,
    { headers: parentHeaders })).json();
  assert.equal(sketchFinal.status, 'completed');
  assert.equal(sketchFinal.reply, '我猜这是小飞机！');
  llmReply = '你好';
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
  const questionHistory = await fetch(`${base}/api/admin/question-history?deviceId=tank-001`,
    { headers: adminHeaders });
  assert.equal(questionHistory.status, 200);
  const questionPage = await questionHistory.json();
  assert.equal(questionPage.questions[0].question, '测试成功');
  assert.equal(questionPage.questions[0].answer, '你好');
  const archiveOff = await fetch(`${base}/api/admin/question-history`, {
    method: 'PUT', headers: adminHeaders,
    body: JSON.stringify({ deviceId: 'tank-001', enabled: false, retentionDays: 7 }) });
  assert.equal(archiveOff.status, 200);
  const afterOptOut = await fetch(`${base}/api/admin/question-history?deviceId=tank-001`,
    { headers: adminHeaders });
  assert.deepEqual((await afterOptOut.json()).questions, []);
  const moodState = await fetch(`${base}/api/device/v1/mood`, {
    headers: deviceHeaders,
  });
  assert.equal(moodState.status, 200);
  const moodBody = await moodState.json();
  assert.equal(moodBody.version, 1);
  assert.match(moodBody.band,
    /^(morning|midday|afternoon|early_evening|evening|late_night)$/);
  assert.equal(moodBody.thought.key, 'cloud');
  assert.equal(moodBody.thought.text, '小云朵慢慢飘过来啦');
  assert.equal(moodBody.thought.displayText, '软绵绵');
  const adminMood = await fetch(`${base}/api/admin/mood?deviceId=tank-001`, {
    headers: adminHeaders,
  });
  assert.equal(adminMood.status, 200);
  assert.equal((await adminMood.json()).events[0].source, 'speech');
  assert.match(llmRequests.at(-1).messages[1].content, /好奇心=78，亲近感=84，话语量=46/);
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
  const afterExpressiveMood = await fetch(`${base}/api/device/v1/mood`, {
    headers: deviceHeaders,
  });
  assert.equal(afterExpressiveMood.status, 200);
  const afterExpressiveMoodBody = await afterExpressiveMood.json();
  assert.equal(afterExpressiveMoodBody.mood.label, moodBody.mood.label,
    'sentence emotion must not overwrite the needs-derived long-term mood');
  assert.equal(afterExpressiveMoodBody.mood.valence, moodBody.mood.valence);
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
  assert.equal(llmRequests.at(-1).messages.filter((item) => item.role === 'assistant').length, 7);

  asrText = '画个小汽车';
  const modelDrawing = { durationMs: 15000,
    svg: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 240">' +
      '<rect x="0" y="0" width="240" height="240" fill="#071018"/>' +
      '<rect x="40" y="95" width="160" height="70" rx="16" fill="#59f3e8"/>' +
      '<circle cx="78" cy="172" r="20" fill="#fff1c7"/>' +
      '<circle cx="162" cy="172" r="20" fill="#fff1c7"/></svg>' };
  drawingProposalReply = JSON.stringify({ drawing: modelDrawing });
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
  assert.equal(drawing.headers.get('x-drawing-duration-ms'), '15000');
  const drawingJpeg = Buffer.from(await drawing.arrayBuffer());
  assert.deepEqual([...drawingJpeg.subarray(0, 2)], [0xff, 0xd8]);
  const drawingReplay = await fetch(`${base}/api/device/v1/drawing?traceId=${drawTrace}`,
    { headers: deviceHeaders });
  assert.equal(drawingReplay.status, 409);
  const drawingAudit = new DatabaseSync(config.dbPath);
  assert.equal(drawingAudit.prepare('SELECT COUNT(*) AS count FROM ai_action_execution WHERE trace_id=?')
    .get(drawTrace).count, 0, 'drawing must not authorize motion');
  drawingAudit.close();
  drawingProposalReply = JSON.stringify({ drawing: modelDrawing });
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
  asrText = '给我讲一个毛线球的故事';
  llmReply = JSON.stringify({ reply: '故事里滚来一只追毛线的小猫。', emotion: 'happy',
    intensity: 72, action: null, camera: null,
    drawing: { subject: '一只追毛线的小猫' } });
  const autonomousDrawing = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(autonomousDrawing.status, 200);
  assert.equal(autonomousDrawing.headers.get('x-ai-action-type'), 'draw',
    'a valid model drawing decision should execute without an explicit draw command');
  const autonomousDrawingTrace = autonomousDrawing.headers.get('x-turn-trace-id');
  await autonomousDrawing.arrayBuffer();
  const autonomousScene = await fetch(`${base}/api/device/v1/drawing?traceId=${autonomousDrawingTrace}`,
    { headers: deviceHeaders });
  assert.equal(autonomousScene.status, 200);
  assert.equal(autonomousScene.headers.get('content-type'), 'image/jpeg');
  assert.ok((await autonomousScene.arrayBuffer()).byteLength > 256);
  asrText = '画个小汽车';
  llmReply = JSON.stringify({ reply: '再画一辆。', emotion: 'happy',
    intensity: 70, action: null, camera: null, drawing: null });
  drawingProposalReplies = [JSON.stringify({ drawing: { durationMs: 15000,
    items: [{ emoji: '🚗', x: 120, y: 120 }] } }),
  JSON.stringify({ drawing: modelDrawing })];
  const beforeRepair = llmRequests.length;
  const repairedDrawing = await fetch(`${base}/api/device/v1/conversation/stream?format=pcm&sampleRate=16000&wire=framed-v1`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(repairedDrawing.status, 200);
  assert.equal(repairedDrawing.headers.get('x-ai-action-type'), 'draw');
  await repairedDrawing.arrayBuffer();
  assert.equal(llmRequests.slice(beforeRepair).filter((request) =>
    request.messages[0]?.content.includes('儿童矢量插画的画师')).length, 2,
  'one invalid drawing may be repaired once, without emoji fallback');
  drawingProposalReplies = null;
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
  assert.equal(deterministicDrive.headers.get('x-ai-action-mode'), 'shadow');
  assert.equal(deterministicDrive.headers.get('x-ai-action-direction'), null);
  assert.equal(deterministicDrive.headers.get('x-ai-action-duration-ms'), null);
  await deterministicDrive.arrayBuffer();
  asrText = '为什么不能前进';
  llmReply = JSON.stringify({ reply: '我们先聊聊', action: {
    type: 'drive', direction: 'forward', durationMs: 3000, lightMode: 'rainbow' } });
  const indirect = await fetch(`${base}/api/device/v1/conversation?format=pcm&sampleRate=16000`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/pcm' }, body: audio,
  });
  assert.equal(indirect.headers.get('x-ai-action-mode'), 'shadow');
  assert.equal(indirect.headers.get('x-ai-action-status'), 'rejected');
  assert.equal(indirect.headers.get('x-ai-action-type'), 'none');
  assert.equal(indirect.headers.get('x-ai-action-direction'), null);
  await indirect.arrayBuffer();
  const indirectAudit = new DatabaseSync(config.dbPath);
  assert.equal(indirectAudit.prepare('SELECT COUNT(*) AS count FROM ai_action_execution WHERE trace_id=?')
    .get(indirect.headers.get('x-turn-trace-id')).count, 0,
  'a question about movement must not create a motor authorization');
  indirectAudit.close();

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
  assert.equal(touchCannotDrive.headers.get('x-ai-action-status'), 'none');
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

  const publicReleases = await fetch(`${base}/api/public/v1/releases`);
  const publicRelease = (await publicReleases.json()).releases[0];
  assert.equal(publicRelease.version, '1.2.3');
  assert.equal(publicRelease.board, 'baby-tank-s3');
  assert.equal(publicRelease.downloadUrl, `/api/public/v1/releases/${release.id}/download`);
  const publicDownload = await fetch(`${base}${publicRelease.downloadUrl}`, { headers: { range: 'bytes=0-15' } });
  assert.equal(publicDownload.status, 206);
  assert.equal((await publicDownload.arrayBuffer()).byteLength, 16);

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
  const staleIdleSocket = new WebSocket(
    `ws://127.0.0.1:${server.address().port}/api/device/v1/conversation/ws`,
    { headers: deviceHeaders });
  await once(staleIdleSocket, 'open');
  const staleClosed = once(staleIdleSocket, 'close');
  const replacementSocket = new WebSocket(
    `ws://127.0.0.1:${server.address().port}/api/device/v1/conversation/ws`,
    { headers: deviceHeaders });
  await once(replacementSocket, 'open');
  await staleClosed;
  replacementSocket.close();
  await once(replacementSocket, 'close');
  const voiceSocket = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/v1/conversation/ws`,
    { headers: deviceHeaders });
  await once(voiceSocket, 'open');
  const asrBytesBeforeSocket = asrAudioBytes;
  async function websocketVoiceTurn(packet = audio) {
    const received = [];
    let replyHeaders = null;
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
          replyHeaders = message.headers;
        } else if (message.type === 'turn_end') {
          voiceSocket.off('message', onMessage);
          resolve({ audio: Buffer.concat(received), headers: replyHeaders });
        }
      };
      voiceSocket.on('message', onMessage);
    });
    voiceSocket.send(JSON.stringify({ type: 'listen', state: 'start',
      curiosity: 78, warmth: 84, talkativeness: 46 }));
    voiceSocket.send(packet, { binary: true });
    voiceSocket.send(JSON.stringify({ type: 'listen', state: 'stop' }));
    return finished;
  }
  const batchedPcm = Buffer.concat([audio, audio, audio]).subarray(0, 8_000);
  const firstSocketTurn = await websocketVoiceTurn(batchedPcm);
  const secondSocketTurn = await websocketVoiceTurn();
  assert.equal(firstSocketTurn.audio[0], 0x50);
  assert.equal(secondSocketTurn.audio[0], 0x50);
  assert.equal(asrAudioBytes - asrBytesBeforeSocket, batchedPcm.length + audio.length,
    'a batched PCM frame and the following turn must share one authenticated WebSocket without losing microphone frames');

  asrText = '往后退一步';
  llmReply = JSON.stringify({ reply: '好，往后走一点。', emotion: 'calm',
    action: null });
  driveRouterReply = JSON.stringify({ action: { type: 'drive', direction: 'backward',
    durationMs: 2000 } });
  const socketDrive = await websocketVoiceTurn();
  assert.equal(socketDrive.headers['x-ai-action-version'], '4');
  assert.equal(socketDrive.headers['x-ai-action-mode'], 'execute');
  assert.equal(socketDrive.headers['x-ai-action-status'], 'approved');
  assert.equal(socketDrive.headers['x-ai-action-direction'], 'backward');
  assert.equal(socketDrive.headers['x-ai-action-duration-ms'], '2000');
  assert.equal(socketDrive.audio[0], 0x50);
  driveRouterReply = null;

  asrText = '画个小汽车';
  llmReply = JSON.stringify({ reply: '给你画辆小汽车。', emotion: 'happy',
    action: null, camera: null, drawing: null });
  drawingProposalReply = JSON.stringify({ drawing: modelDrawing });
  const socketDraw = await websocketVoiceTurn();
  assert.equal(socketDraw.headers['x-ai-action-version'], '6');
  assert.equal(socketDraw.headers['x-ai-action-type'], 'draw');
  assert.equal(socketDraw.headers['x-ai-action-status'], 'approved');
  assert.equal(socketDraw.audio[0], 0x50);
  const socketDrawing = await fetch(`${base}/api/device/v1/drawing?traceId=${
    socketDraw.headers['x-turn-trace-id']}`, { headers: deviceHeaders });
  assert.equal(socketDrawing.status, 200);
  assert.equal(socketDrawing.headers.get('x-drawing-duration-ms'), '15000');
  assert.deepEqual([...Buffer.from(await socketDrawing.arrayBuffer()).subarray(0, 2)],
    [0xff, 0xd8]);
  const socketTextId = 'dd617c68-093b-4b63-9a5b-a44fbd5f73e0';
  const textAvailable = new Promise((resolve) => {
    const onMessage = (payload, binary) => {
      if (binary) return;
      const message = JSON.parse(String(payload));
      if (message.type !== 'text_available') return;
      voiceSocket.off('message', onMessage);
      resolve(message);
    };
    voiceSocket.on('message', onMessage);
  });
  const socketTextSubmit = await fetch(`${base}/api/parent/v1/text-turns`, {
    method: 'POST', headers: { ...parentHeaders, 'content-type': 'application/json' },
    body: JSON.stringify({ requestId: socketTextId, text: '讲个小故事' }),
  });
  assert.equal(socketTextSubmit.status, 202);
  const textNotice = await textAvailable;
  assert.equal(textNotice.type, 'text_available');
  assert.equal(textNotice.id, socketTextId);
  llmReply = JSON.stringify({ reply: '从前有一辆勇敢的小坦克。',
    emotion: 'happy', action: null, camera: null });
  const textAudio = [];
  const socketTextFinished = new Promise((resolve, reject) => {
    const onMessage = (payload, binary) => {
      if (binary) { textAudio.push(Buffer.from(payload)); return; }
      const message = JSON.parse(String(payload));
      if (message.type === 'turn_error') {
        voiceSocket.off('message', onMessage);
        reject(new Error(`${message.status}: ${message.code}`));
      } else if (message.type === 'turn_end') {
        voiceSocket.off('message', onMessage);
        resolve();
      }
    };
    voiceSocket.on('message', onMessage);
  });
  voiceSocket.send(JSON.stringify({ type: 'text', state: 'start',
    jobId: socketTextId }));
  await socketTextFinished;
  assert.equal(Buffer.concat(textAudio)[0], 0x50);
  const socketTextBlocked = await fetch(
    `${base}/api/device/v1/text-turns/${socketTextId}/complete`, {
      method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
      body: JSON.stringify({ status: 'blocked' }),
    });
  assert.equal(socketTextBlocked.status, 200);
  voiceSocket.close();
  await once(voiceSocket, 'close');

  async function claimTypedTurn(id, text) {
    const idlePoll = await fetch(`${base}/api/device/v1/text-turns/next?wait=0`,
      { headers: deviceHeaders });
    assert.equal(idlePoll.status, 204);
    const submitted = await fetch(`${base}/api/parent/v1/text-turns`, {
      method: 'POST', headers: { ...parentHeaders, 'content-type': 'application/json' },
      body: JSON.stringify({ requestId: id, text }),
    });
    assert.equal(submitted.status, 202);
    const claimed = await fetch(`${base}/api/device/v1/text-turns/next?wait=0`,
      { headers: deviceHeaders });
    assert.equal((await claimed.json()).id, id);
  }
  async function completeTypedTurn(id) {
    const completed = await fetch(`${base}/api/device/v1/text-turns/${id}/complete`, {
      method: 'POST', headers: { ...deviceHeaders, 'content-type': 'application/json' },
      body: JSON.stringify({ status: 'completed' }),
    });
    assert.equal(completed.status, 200);
  }
  const typedLookId = 'dd617c68-093b-4b63-9a5b-a44fbd5f73fc';
  await claimTypedTurn(typedLookId, '看看眼前是什么');
  const typedLook = await fetch(`${base}/api/device/v1/conversation/text?jobId=${typedLookId}&wire=framed-v1&vision=always`, {
    method: 'POST', headers: deviceHeaders,
  });
  assert.equal(typedLook.status, 200);
  assert.equal(typedLook.headers.get('x-ai-action-type'), 'look');
  const typedLookTrace = typedLook.headers.get('x-turn-trace-id');
  await typedLook.arrayBuffer();
  visionReply = JSON.stringify({ reply: '眼前有一张桌子。', emotion: 'curious',
    intensity: 60, action: null });
  const typedPhotoAnswer = await fetch(`${base}/api/device/v1/vision/describe?traceId=${typedLookTrace}`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'image/jpeg' },
    body: lookJpeg,
  });
  assert.equal(typedPhotoAnswer.status, 200);
  await typedPhotoAnswer.arrayBuffer();
  assert.equal((await (await fetch(`${base}/api/parent/v1/text-turns/${typedLookId}`,
    { headers: parentHeaders })).json()).reply, '眼前有一张桌子。');
  await completeTypedTurn(typedLookId);

  const typedDriveId = 'dd617c68-093b-4b63-9a5b-a44fbd5f73fd';
  await claimTypedTurn(typedDriveId, '往后走一点');
  llmReply = JSON.stringify({ reply: '好，往后走一点。', emotion: 'calm',
    action: { type: 'drive', direction: 'backward', durationMs: 2000 } });
  const typedDrive = await fetch(`${base}/api/device/v1/conversation/text?jobId=${typedDriveId}&wire=framed-v1&vision=always`, {
    method: 'POST', headers: deviceHeaders,
  });
  assert.equal(typedDrive.status, 200);
  assert.equal(typedDrive.headers.get('x-ai-action-direction'), 'backward');
  assert.equal(typedDrive.headers.get('x-ai-action-duration-ms'), '2000');
  await typedDrive.arrayBuffer();
  await completeTypedTurn(typedDriveId);
});
