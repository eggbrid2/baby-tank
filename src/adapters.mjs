import { randomUUID } from 'node:crypto';
import WebSocket from 'ws';

const LLM_DEFAULTS = {
  openai: 'https://api.openai.com/v1',
  deepseek: 'https://api.deepseek.com/v1',
  qwen: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
};

const AUDIO_CONTENT_TYPES = {
  mp3: 'audio/mpeg',
  pcm: 'audio/pcm',
  ogg_opus: 'audio/ogg',
};

function trimTrailingSlash(value) {
  return String(value || '').replace(/\/+$/, '');
}

function providerError(code, status = 502) {
  return Object.assign(new Error(code), { code, status });
}

export function parseProviderOptions(selected) {
  try {
    const parsed = JSON.parse(selected.options_json || '{}');
    return parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : {};
  } catch {
    throw providerError('provider_options_invalid', 500);
  }
}

export function validateChatMessages(messages) {
  if (!Array.isArray(messages) || messages.length < 1 || messages.length > 50) return false;
  let totalCharacters = 0;
  for (const message of messages) {
    if (!message || !['system', 'user', 'assistant', 'tool'].includes(message.role) || typeof message.content !== 'string') return false;
    if (!message.content.length || message.content.length > 32_000) return false;
    totalCharacters += message.content.length;
  }
  return totalCharacters <= 200_000;
}

export async function requestChatCompletion({ selected, credentials, request, fetchImpl = globalThis.fetch,
  timeoutMs = 120_000 }) {
  if (!credentials.apiKey) throw providerError('llm_api_key_missing', 503);
  const options = parseProviderOptions(selected);
  const baseUrl = trimTrailingSlash(selected.base_url || LLM_DEFAULTS[selected.provider]);
  if (!baseUrl || !selected.model) throw providerError('llm_endpoint_or_model_missing', 503);
  const dialect = selected.provider === 'custom' ? (options.llmDialect || 'openai') : selected.provider;
  const thinkingMode = ['default', 'on', 'off'].includes(options.thinkingMode) ? options.thinkingMode : 'default';
  const allowedEfforts = new Set(['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max']);
  const configuredEffort = allowedEfforts.has(options.reasoningEffort) ? options.reasoningEffort : null;
  const configuredMaxTokens = Number(options.maxTokens);
  const configuredTemperature = Number(options.temperature);
  const payload = {
    model: selected.model,
    messages: request.messages,
    stream: Boolean(request.stream),
  };
  let effectiveEffort = configuredEffort;
  if (thinkingMode === 'off') effectiveEffort = 'none';
  if (thinkingMode === 'on' && !effectiveEffort && dialect === 'openai') effectiveEffort = 'medium';

  if (dialect === 'deepseek') {
    if (thinkingMode !== 'default') payload.thinking = { type: thinkingMode === 'on' ? 'enabled' : 'disabled' };
    if (effectiveEffort && effectiveEffort !== 'none') payload.reasoning_effort = effectiveEffort;
  } else if (dialect === 'qwen') {
    if (effectiveEffort) payload.reasoning_effort = effectiveEffort;
    else if (thinkingMode === 'on') payload.enable_thinking = true;
  } else if (effectiveEffort) {
    payload.reasoning_effort = effectiveEffort;
  }

  const maxTokens = Number.isInteger(configuredMaxTokens) && configuredMaxTokens > 0 ? configuredMaxTokens : request.max_tokens;
  if (Number.isInteger(maxTokens)) {
    const bounded = Math.min(32_768, Math.max(1, maxTokens));
    if (dialect === 'openai') payload.max_completion_tokens = bounded;
    else payload.max_tokens = bounded;
  }
  const temperature = Number.isFinite(configuredTemperature) && options.temperature !== '' ? configuredTemperature : request.temperature;
  const reasoningEnabled = thinkingMode === 'on' || (effectiveEffort && effectiveEffort !== 'none');
  if (!reasoningEnabled && Number.isFinite(temperature)) payload.temperature = Math.min(2, Math.max(0, temperature));

  try {
    return await fetchImpl(`${baseUrl}/chat/completions`, {
      method: 'POST',
      headers: { authorization: `Bearer ${credentials.apiKey}`, 'content-type': 'application/json', 'user-agent': 'baby-tank-cloud/0.2' },
      body: JSON.stringify(payload),
      signal: AbortSignal.timeout(timeoutMs),
    });
  } catch (error) {
    throw providerError(error.name === 'TimeoutError' ? 'llm_upstream_timeout' : 'llm_upstream_unreachable');
  }
}

function qwenAsrUrl(selected, credentials) {
  if (selected.base_url) {
    const url = new URL(selected.base_url);
    if (url.protocol === 'https:') url.protocol = 'wss:';
    return url.toString();
  }
  if (credentials.workspaceId) return `wss://${credentials.workspaceId}.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference`;
  return 'wss://dashscope.aliyuncs.com/api-ws/v1/inference';
}

function delay(milliseconds) {
  return milliseconds > 0 ? new Promise((resolve) => setTimeout(resolve, milliseconds)) : Promise.resolve();
}

function recognizeWithQwenFrames({ selected, credentials, audioChunks, format, sampleRate, webSocketFactory, frameDelayMs = 0 }) {
  if (!credentials.apiKey) throw providerError('asr_api_key_missing', 503);
  const options = parseProviderOptions(selected);
  const model = selected.model || 'paraformer-realtime-v2';
  const taskId = randomUUID();
  const headers = { Authorization: `Bearer ${credentials.apiKey}`, 'User-Agent': 'baby-tank-cloud/0.2' };
  if (credentials.workspaceId) headers['X-DashScope-WorkSpace'] = credentials.workspaceId;
  const createSocket = webSocketFactory || ((url, socketOptions) => new WebSocket(url, socketOptions));

  return new Promise((resolve, reject) => {
    let settled = false;
    let started = false;
    let lastPartial = '';
    const sentences = [];
    const socket = createSocket(qwenAsrUrl(selected, credentials), { headers });
    const timeout = setTimeout(() => finish(providerError('asr_upstream_timeout')), 180_000);

    function finish(error, result) {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      try { socket.close(); } catch {}
      if (error) reject(error);
      else resolve(result);
    }

    async function sendAudio() {
      const pcmBytesPer100ms = Math.max(1_024, Math.min(16_000, Math.round(sampleRate * 2 / 10)));
      const frameBytes = format === 'pcm' ? pcmBytesPer100ms : 8_192;
      try {
        let pending = Buffer.alloc(0);
        let sentBytes = 0;
        for await (const chunk of audioChunks) {
          if (settled) return;
          if (!Buffer.isBuffer(chunk) || chunk.length === 0) continue;
          pending = pending.length ? Buffer.concat([pending, chunk]) : chunk;
          while (pending.length >= frameBytes) {
            socket.send(pending.subarray(0, frameBytes));
            sentBytes += frameBytes;
            pending = pending.subarray(frameBytes);
            if (frameDelayMs > 0) await delay(frameDelayMs);
            if (settled) return;
          }
        }
        if (pending.length > 0) {
          socket.send(pending);
          sentBytes += pending.length;
        }
        if (sentBytes === 0 || (format === 'pcm' && sentBytes % 2 !== 0)) {
          finish(providerError('asr_audio_invalid', 400));
          return;
        }
        socket.send(JSON.stringify({ header: { action: 'finish-task', task_id: taskId, streaming: 'duplex' }, payload: { input: {} } }));
      } catch (error) {
        finish(error.status ? error : providerError('asr_upstream_connection_lost'));
      }
    }

    socket.on('open', () => {
      socket.send(JSON.stringify({
        header: { action: 'run-task', task_id: taskId, streaming: 'duplex' },
        payload: {
          task_group: 'audio', task: 'asr', function: 'recognition', model,
          parameters: {
            format, sample_rate: sampleRate,
            disfluency_removal_enabled: Boolean(options.disfluencyRemoval),
            language_hints: Array.isArray(options.languageHints) ? options.languageHints.slice(0, 4) : ['zh', 'en'],
            semantic_punctuation_enabled: false,
            max_sentence_silence: Number.isInteger(options.maxSentenceSilence) ? options.maxSentenceSilence : 800,
            punctuation_prediction_enabled: true,
            inverse_text_normalization_enabled: true,
          },
          input: {},
        },
      }));
    });
    socket.on('message', (raw) => {
      let message;
      try { message = JSON.parse(Buffer.isBuffer(raw) ? raw.toString('utf8') : String(raw)); } catch { return; }
      const event = message.header?.event;
      if (event === 'task-started' && !started) {
        started = true;
        void sendAudio();
      } else if (event === 'result-generated') {
        const sentence = message.payload?.output?.sentence;
        if (typeof sentence?.text === 'string') {
          lastPartial = sentence.text;
          if (sentence.sentence_end) sentences.push({
            text: sentence.text,
            beginTime: sentence.begin_time,
            endTime: sentence.end_time,
          });
        }
      } else if (event === 'task-failed') {
        finish(providerError('asr_upstream_rejected'));
      } else if (event === 'task-finished') {
        const text = sentences.length ? sentences.map((item) => item.text).join('') : lastPartial;
        finish(null, { text, sentences, provider: 'qwen', model });
      }
    });
    socket.on('error', () => finish(providerError('asr_upstream_unreachable')));
    socket.on('close', () => {
      if (!settled) finish(providerError(started ? 'asr_upstream_connection_lost' : 'asr_upstream_unreachable'));
    });
  });
}

export function recognizeWithQwen({ audio, frameDelayMs = 100, ...options }) {
  async function* frames() {
    for (let offset = 0; offset < audio.length; offset += 3_200) {
      yield audio.subarray(offset, Math.min(offset + 3_200, audio.length));
    }
  }
  return recognizeWithQwenFrames({ ...options, audioChunks: frames(), frameDelayMs });
}

export function recognizeStreamWithQwen({ audioChunks, ...options }) {
  return recognizeWithQwenFrames({ ...options, audioChunks, frameDelayMs: 0 });
}

async function* decodeVolcengineSse(body) {
  if (!body) throw providerError('tts_upstream_empty_response');
  const decoder = new TextDecoder();
  let pending = '';
  let producedAudio = false;
  for await (const chunk of body) {
    pending += decoder.decode(chunk, { stream: true });
    const lines = pending.split(/\r?\n/);
    pending = lines.pop() || '';
    for (const line of lines) {
      if (!line.startsWith('data:')) continue;
      let event;
      try { event = JSON.parse(line.slice(5).trim()); } catch { continue; }
      if (![0, 20_000_000, undefined].includes(event.code)) throw providerError('tts_upstream_rejected');
      if (event.data) {
        producedAudio = true;
        yield Buffer.from(event.data, 'base64');
      }
    }
  }
  pending += decoder.decode();
  if (pending.startsWith('data:')) {
    try {
      const event = JSON.parse(pending.slice(5).trim());
      if (![0, 20_000_000, undefined].includes(event.code)) throw providerError('tts_upstream_rejected');
      if (event.data) {
        producedAudio = true;
        yield Buffer.from(event.data, 'base64');
      }
    } catch (error) {
      if (error.code) throw error;
    }
  }
  if (!producedAudio) throw providerError('tts_upstream_empty_response');
}

export async function synthesizeWithVolcengine({ selected, credentials, text, deviceId, fetchImpl = globalThis.fetch }) {
  if (!credentials.apiKey && !(credentials.appId && credentials.accessToken)) throw providerError('tts_credentials_missing', 503);
  const options = parseProviderOptions(selected);
  const format = ['mp3', 'pcm', 'ogg_opus'].includes(options.audioFormat) ? options.audioFormat : 'mp3';
  const sampleRate = [8_000, 16_000, 22_050, 24_000, 32_000, 44_100, 48_000].includes(Number(options.sampleRate))
    ? Number(options.sampleRate) : 24_000;
  const speaker = String(options.speaker || '').trim();
  if (!speaker) throw providerError('tts_speaker_missing', 503);
  const headers = {
    'content-type': 'application/json',
    'x-api-resource-id': credentials.resourceId || 'seed-tts-2.0',
    'x-api-request-id': randomUUID(),
    'user-agent': 'baby-tank-cloud/0.2',
  };
  if (credentials.apiKey) headers['x-api-key'] = credentials.apiKey;
  else {
    headers['x-api-app-id'] = credentials.appId;
    headers['x-api-access-key'] = credentials.accessToken;
  }
  const endpoint = selected.base_url || 'https://openspeech.bytedance.com/api/v3/tts/unidirectional/sse';
  let upstream;
  try {
    upstream = await fetchImpl(endpoint, {
      method: 'POST', headers,
      body: JSON.stringify({
        user: { uid: String(deviceId).slice(0, 128) },
        req_params: {
          text, speaker,
          audio_params: { format, sample_rate: sampleRate,
            speech_rate: Number(options.speechRate) || 0, loudness_rate: Number(options.loudnessRate) || 0 },
        },
      }),
      signal: AbortSignal.timeout(90_000),
    });
  } catch (error) {
    throw providerError(error.name === 'TimeoutError' ? 'tts_upstream_timeout' : 'tts_upstream_unreachable');
  }
  if (!upstream.ok) {
    try { await upstream.body?.cancel(); } catch {}
    throw providerError('tts_upstream_rejected');
  }
  return {
    format,
    sampleRate,
    contentType: AUDIO_CONTENT_TYPES[format],
    audio: decodeVolcengineSse(upstream.body),
  };
}
