import { PassThrough, Readable } from 'node:stream';
import { WebSocketServer, WebSocket } from 'ws';

const VOICE_PATH = '/api/device/v1/conversation/ws';
const UPLOAD_TIMEOUT_MS = 20_000;
const REPLY_TIMEOUT_MS = 120_000;
const IDLE_TIMEOUT_MS = 30_000;
const MAX_PACKET_BYTES = 4_096;

function validTrait(value) {
  return Number.isInteger(value) && value >= 0 && value <= 100;
}

function send(socket, data, binary = false) {
  return new Promise((resolve, reject) => {
    if (socket.readyState !== WebSocket.OPEN) return reject(new Error('voice_socket_closed'));
    socket.send(data, { binary }, (error) => error ? reject(error) : resolve());
  });
}

function rejectUpgrade(socket, status) {
  const label = status === 401 ? 'Unauthorized' : status === 409 ? 'Conflict' : 'Bad Request';
  socket.end(`HTTP/1.1 ${status} ${label}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
}

// This transport bridges a persistent device socket to the existing, tested
// one-turn conversation handler on loopback. ASR, AI actions, TTS and memory
// therefore retain exactly the same authorization and behavior as HTTP turns.
export function attachVoiceSocket(server, { authenticate, speechMaxBytes }) {
  const sockets = new Map();
  const websocketServer = new WebSocketServer({ noServer: true, maxPayload: MAX_PACKET_BYTES });

  server.on('upgrade', (request, socket, head) => {
    let path;
    try { path = new URL(request.url, 'http://localhost').pathname; }
    catch { return rejectUpgrade(socket, 400); }
    if (path !== VOICE_PATH) return rejectUpgrade(socket, 400);
    const device = authenticate(request.headers.authorization);
    if (!device) return rejectUpgrade(socket, 401);
    if (sockets.has(device.id)) return rejectUpgrade(socket, 409);
    websocketServer.handleUpgrade(request, socket, head, (ws) => {
      sockets.set(device.id, ws);
      websocketServer.emit('connection', ws, request, device);
    });
  });

  websocketServer.on('connection', (socket, request, device) => {
    let turn = null;
    let idleTimer = null;
    const authorization = request.headers.authorization;
    const resetIdle = () => {
      clearTimeout(idleTimer);
      if (!turn) idleTimer = setTimeout(() => socket.close(1000, 'voice session idle'), IDLE_TIMEOUT_MS);
    };
    const closeTurn = () => {
      if (!turn) return;
      clearTimeout(turn.timer);
      turn.controller.abort();
      turn.audio.destroy();
      turn = null;
      resetIdle();
    };
    const fail = async (status, code) => {
      closeTurn();
      try { await send(socket, JSON.stringify({ type: 'turn_error', status, code })); }
      catch { socket.terminate(); }
    };
    const runTurn = async (active, query) => {
      try {
        const address = server.address();
        if (!address || typeof address === 'string') throw new Error('voice_listener_unavailable');
        const url = new URL(`http://127.0.0.1:${address.port}/api/device/v1/conversation/stream`);
        for (const [key, value] of Object.entries(query)) url.searchParams.set(key, String(value));
        const response = await fetch(url, {
          method: 'POST',
          headers: { authorization, 'content-type': 'audio/pcm' },
          body: Readable.toWeb(active.audio), duplex: 'half',
          signal: active.controller.signal,
        });
        if (turn !== active) return;
        active.replyAt = performance.now();
        const headers = {};
        for (const [key, value] of response.headers) {
          if (key.startsWith('x-ai-') || key === 'x-turn-trace-id' ||
              key === 'x-audio-format' || key === 'x-audio-sample-rate' ||
              key === 'x-babytank-audio-framing') headers[key] = value;
        }
        await send(socket, JSON.stringify({ type: 'reply', status: response.status, headers }));
        if (response.ok && response.body) {
          for await (const chunk of Readable.fromWeb(response.body)) {
            if (turn !== active) return;
            active.firstAudioAt ??= performance.now();
            await send(socket, chunk, true);
          }
        } else if (response.body) {
          await response.body.cancel();
        }
        if (turn !== active) return;
        const finishedAt = performance.now();
        console.info(JSON.stringify({ event: 'voice_socket.turn', device: device.id,
          status: response.status, inputBytes: active.bytes,
          uploadMs: Math.round((active.uploadDoneAt || finishedAt) - active.startedAt),
          replyAfterUploadMs: active.uploadDoneAt
            ? Math.round(active.replyAt - active.uploadDoneAt) : null,
          firstAudioAfterUploadMs: active.uploadDoneAt && active.firstAudioAt
            ? Math.round(active.firstAudioAt - active.uploadDoneAt) : null,
          totalMs: Math.round(finishedAt - active.startedAt) }));
        clearTimeout(active.timer);
        turn = null;
        await send(socket, JSON.stringify({ type: 'turn_end' }));
        resetIdle();
      } catch (error) {
        if (turn === active) await fail(502, error.name === 'AbortError'
          ? 'voice_turn_timeout' : 'voice_bridge_failed');
      }
    };

    socket.on('message', (payload, binary) => {
      if (binary) {
        if (!turn || turn.uploadDone || payload.length === 0 ||
            payload.length > MAX_PACKET_BYTES || payload.length % 2 !== 0 ||
            turn.bytes + payload.length > speechMaxBytes) {
          void fail(400, 'voice_audio_invalid');
          return;
        }
        turn.bytes += payload.length;
        if (!turn.audio.write(payload)) socket.pause();
        return;
      }
      let message;
      try { message = JSON.parse(String(payload)); }
      catch { void fail(400, 'voice_message_invalid'); return; }
      if (message.type === 'listen' && message.state === 'start') {
        if (turn || !validTrait(message.curiosity) ||
            !validTrait(message.warmth) || !validTrait(message.talkativeness)) {
          void fail(409, 'voice_turn_invalid');
          return;
        }
        clearTimeout(idleTimer);
        const active = {
          audio: new PassThrough({ highWaterMark: 16 * 1024 }),
          controller: new AbortController(), bytes: 0, uploadDone: false,
          timer: null, startedAt: performance.now(), uploadDoneAt: null,
          replyAt: null, firstAudioAt: null,
        };
        active.audio.on('drain', () => socket.resume());
        active.timer = setTimeout(() => {
          if (turn === active) void fail(408, 'voice_turn_timeout');
        }, UPLOAD_TIMEOUT_MS);
        turn = active;
        void runTurn(active, { format: 'pcm', sampleRate: 16000,
          wire: 'framed-v1', vision: 'always', curiosity: message.curiosity,
          warmth: message.warmth, talkativeness: message.talkativeness });
      } else if (message.type === 'listen' && message.state === 'stop') {
        if (!turn || turn.uploadDone || turn.bytes < 2) {
          void fail(400, 'voice_audio_invalid');
          return;
        }
        turn.uploadDone = true;
        turn.uploadDoneAt = performance.now();
        clearTimeout(turn.timer);
        turn.timer = setTimeout(() => {
          if (turn) void fail(504, 'voice_reply_timeout');
        }, REPLY_TIMEOUT_MS);
        turn.audio.end();
      } else if (message.type === 'abort') {
        closeTurn();
      } else {
        void fail(400, 'voice_message_invalid');
      }
    });
    socket.on('close', () => {
      closeTurn();
      clearTimeout(idleTimer);
      if (sockets.get(device.id) === socket) sockets.delete(device.id);
    });
    socket.on('error', () => socket.terminate());
    void send(socket, JSON.stringify({ type: 'hello', version: 1,
      transport: 'websocket', format: 'pcm16le', sampleRate: 16000 }));
    resetIdle();
  });

  server.on('close', () => {
    for (const socket of sockets.values()) socket.terminate();
    websocketServer.close();
  });
}
