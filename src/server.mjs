import { createServer } from 'node:http';
import { closeSync, createReadStream, createWriteStream, mkdirSync, openSync, readSync, renameSync, rmSync, statSync } from 'node:fs';
import { dirname, extname, join, resolve } from 'node:path';
import { pipeline } from 'node:stream/promises';
import { Readable } from 'node:stream';
import { createHash, randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { loadConfig } from './config.mjs';
import { openDatabase } from './db.mjs';
import { compareVersions, createSecretBox, hashPassword, hashToken, isSafeName, json, newToken, now, parseCookies, readBody, readJson, verifyPassword } from './lib.mjs';
import { recognizeWithQwen, recognizeStreamWithQwen, requestChatCompletion, synthesizeWithVolcengine, validateChatMessages } from './adapters.mjs';
import { conversationMessages, drawingMessages, explicitDrawIntent, explicitDriveIntent,
  explicitLookIntent, explicitSleepIntent, parseConversationReply,
  parseDrawingProposal } from './conversation.mjs';
import { readConversationMemory, saveConversationMemory } from './conversation_memory.mjs';
import { attachVoiceSocket } from './voice_ws.mjs';
import { explicitExploreIntent, parseVisionStep, visionPrompt, visionAnswerPrompt,
  EXPLORATION_MAX_STEPS, EXPLORATION_MAX_TOTAL_MS } from './exploration.mjs';
import { rasterizeDrawing } from './drawing.mjs';
import { DEFAULT_MOOD, advanceMood, moodHeaders, normalizeMood } from './mood.mjs';

const webRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../public');
const allowedProviders = {
  llm: new Set(['qwen', 'openai', 'deepseek', 'custom']),
  asr: new Set(['qwen', 'volcengine', 'tencent', 'custom']),
  tts: new Set(['qwen', 'volcengine', 'tencent', 'custom']),
};

const PCM_FRAME_BYTES = 4096;

async function writePcmFrame(response, type, payload) {
  if (response.destroyed) throw new Error('conversation_client_disconnected');
  const frame = Buffer.allocUnsafe(5 + payload.length);
  frame[0] = type;
  frame.writeUInt32BE(payload.length, 1);
  payload.copy(frame, 5);
  if (response.write(frame)) return;
  await new Promise((resolve, reject) => {
    const cleanup = () => {
      response.off('drain', drained);
      response.off('close', closed);
      response.off('error', failed);
    };
    const drained = () => { cleanup(); resolve(); };
    const closed = () => { cleanup(); reject(new Error('conversation_client_disconnected')); };
    const failed = (error) => { cleanup(); reject(error); };
    response.once('drain', drained);
    response.once('close', closed);
    response.once('error', failed);
  });
}

export function createApp(config) {
  mkdirSync(config.firmwareDir, { recursive: true });
  const db = openDatabase(config.dbPath, { username: config.adminUsername, password: config.adminPassword });
  const secrets = createSecretBox(config.masterKey);
  const loginAttempts = new Map();
  const conversationBusy = new Set();
  // Deliberately volatile: a restart cancels every autonomous group.
  const explorations = new Map();
  // A camera request is valid only for one authenticated, active dialogue turn.
  const visionSessions = new Map();
  const drawingSessions = new Map();

  function rememberConversation(deviceId, userText, replyText) {
    try {
      saveConversationMemory(db, secrets, deviceId, userText, replyText);
    } catch (error) {
      // A storage outage must not cut off a reply whose TTS has started.
      console.error(`conversation memory persistence failed: ${error.message}`);
    }
  }

  function readMoodState(deviceId) {
    const row = db.prepare(`SELECT valence,arousal,dominance,label,intensity,
      thought_key,thought_ciphertext,updated_at FROM device_mood_state
      WHERE device_id=?`).get(deviceId);
    if (!row) return normalizeMood(DEFAULT_MOOD);
    let thought = { key: row.thought_key, text: '' };
    try {
      const decrypted = secrets.decrypt(row.thought_ciphertext);
      try {
        const stored = JSON.parse(decrypted);
        thought = { key: row.thought_key, text: stored.text,
          displayText: stored.displayText };
      } catch {
        thought.text = decrypted;
      }
    } catch {}
    return normalizeMood({ valence: row.valence, arousal: row.arousal,
      dominance: row.dominance, label: row.label, intensity: row.intensity,
      thought,
      updatedAt: row.updated_at });
  }

  function saveMoodState(deviceId, traceId, source, triggerEmotion,
                         triggerIntensity, mood) {
    const state = normalizeMood(mood);
    db.exec('BEGIN IMMEDIATE');
    try {
      db.prepare(`INSERT INTO device_mood_state(device_id,valence,arousal,dominance,
        label,intensity,thought_key,thought_ciphertext,updated_at)
        VALUES (?,?,?,?,?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET
        valence=excluded.valence,arousal=excluded.arousal,dominance=excluded.dominance,
        label=excluded.label,intensity=excluded.intensity,
        thought_key=excluded.thought_key,thought_ciphertext=excluded.thought_ciphertext,
        updated_at=excluded.updated_at`).run(deviceId, state.valence, state.arousal,
        state.dominance, state.label, state.intensity, state.thought.key,
        secrets.encrypt(JSON.stringify({ text: state.thought.text,
          displayText: state.thought.displayText })), state.updatedAt || now());
      db.prepare(`INSERT INTO device_mood_events(device_id,trace_id,source,
        trigger_emotion,trigger_intensity,valence,arousal,dominance,label,
        thought_key,created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)`).run(deviceId,
        traceId || null, source, triggerEmotion, triggerIntensity, state.valence,
        state.arousal, state.dominance, state.label, state.thought.key,
        state.updatedAt || now());
      db.prepare(`DELETE FROM device_mood_events WHERE device_id=? AND id NOT IN
        (SELECT id FROM device_mood_events WHERE device_id=? ORDER BY id DESC LIMIT 200)`)
        .run(deviceId, deviceId);
      db.exec('COMMIT');
    } catch (error) {
      if (db.isTransaction) db.exec('ROLLBACK');
      throw error;
    }
  }

  function rememberMood(deviceId, traceId, source, triggerEmotion,
                        triggerIntensity, mood) {
    try {
      saveMoodState(deviceId, traceId, source, triggerEmotion,
        triggerIntensity, mood);
    } catch (error) {
      console.error(`mood persistence failed: ${error.message}`);
    }
  }

  function logExploration(deviceId, traceId, step, event, data = {}) {
    db.prepare(`INSERT INTO ai_exploration_events(trace_id,device_id,step_index,event,
      direction,duration_ms,light_mode,emotion,detail,created_at)
      VALUES (?,?,?,?,?,?,?,?,?,?)`).run(traceId, deviceId, step, event,
      data.direction || null, data.durationMs || null, data.lightMode || null,
      data.emotion || null, String(data.detail || '').slice(0, 160), now());
  }

  function logCamera(deviceId, traceId, event, detail = '') {
    db.prepare(`INSERT INTO ai_camera_events(trace_id,device_id,event,detail,created_at)
      VALUES (?,?,?,?,?)`).run(traceId, deviceId, event,
      String(detail).slice(0, 120), now());
  }

  const audit = (action, actor, detail = {}) => db.prepare('INSERT INTO audit_log(action, actor, detail_json, created_at) VALUES (?, ?, ?, ?)')
    .run(action, actor, JSON.stringify(detail), now());

  function securityHeaders(res) {
    res.setHeader('x-content-type-options', 'nosniff');
    res.setHeader('x-frame-options', 'DENY');
    res.setHeader('referrer-policy', 'no-referrer');
    res.setHeader('permissions-policy', 'camera=(), microphone=(), geolocation=()');
    res.setHeader('content-security-policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'");
    res.setHeader('cache-control', 'no-store');
  }

  function adminSession(req) {
    const token = parseCookies(req.headers.cookie).bt_session;
    if (!token) return null;
    return db.prepare(`SELECT s.token_hash, s.csrf_token, s.expires_at, u.id AS user_id, u.username
      FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=? AND s.expires_at>?`)
      .get(hashToken(token), now()) || null;
  }

  function requireAdmin(req, res, write = false) {
    const session = adminSession(req);
    if (!session) {
      json(res, 401, { error: 'authentication_required' });
      return null;
    }
    if (write && req.headers['x-csrf-token'] !== session.csrf_token) {
      json(res, 403, { error: 'invalid_csrf_token' });
      return null;
    }
    return session;
  }

  function authenticateDeviceAuthorization(value) {
    const authorization = String(value || '');
    let token = authorization.replace(/^Bearer\s+/i, '');
    let claimedDeviceId = null;
    if (/^Basic\s+/i.test(authorization)) {
      const decoded = Buffer.from(authorization.replace(/^Basic\s+/i, ''), 'base64').toString('utf8');
      const separator = decoded.indexOf(':');
      claimedDeviceId = decoded.slice(0, separator);
      token = separator >= 0 ? decoded.slice(separator + 1) : '';
    }
    if (!token) return null;
    const device = db.prepare('SELECT * FROM devices WHERE token_hash=? AND revoked_at IS NULL').get(hashToken(token));
    return device && (!claimedDeviceId || device.device_id === claimedDeviceId) ? device : null;
  }

  function requireDevice(req, res) {
    if (!req.headers.authorization) {
      json(res, 401, { error: 'device_authentication_required' },
        { 'www-authenticate': 'Basic realm="baby-tank-device"' });
      return null;
    }
    const device = authenticateDeviceAuthorization(req.headers.authorization);
    if (!device) {
      json(res, 401, { error: 'invalid_device_token' }, { 'www-authenticate': 'Basic realm="baby-tank-device"' });
      return null;
    }
    return device;
  }

  function publicUrl(req) {
    if (config.publicBaseUrl) return config.publicBaseUrl;
    return `${req.headers['x-forwarded-proto'] || 'http'}://${req.headers.host}`;
  }

  function activeProvider(kind) {
    return db.prepare('SELECT * FROM provider_configs WHERE kind=? AND enabled=1 ORDER BY updated_at DESC LIMIT 1').get(kind);
  }

  function providerCredentials(selected) {
    try {
      return JSON.parse(secrets.decrypt(selected?.secret_ciphertext) || '{}');
    } catch {
      throw Object.assign(new Error('provider_credentials_invalid'), { status: 500 });
    }
  }

  async function handleApi(req, res, url) {
    if (req.method === 'GET' && url.pathname === '/healthz') {
      return json(res, 200, { status: 'ok', service: 'baby-tank-cloud', time: now() });
    }

    if (req.method === 'GET' && url.pathname === '/api/device/v1/mood') {
      const device = requireDevice(req, res);
      if (!device) return;
      const mood = readMoodState(device.id);
      return json(res, 200, { version: 1, mood: {
        label: mood.label, intensity: mood.intensity,
        valence: mood.valence, arousal: mood.arousal,
        dominance: mood.dominance,
      }, thought: mood.thought, updatedAt: mood.updatedAt });
    }

    if (req.method === 'POST' && url.pathname === '/api/auth/login') {
      const ip = req.socket.remoteAddress || 'unknown';
      const previous = loginAttempts.get(ip);
      const state = previous && Date.now() < previous.resetAt ? previous : { count: 0, resetAt: 0 };
      if (Date.now() < state.resetAt && state.count >= 8) return json(res, 429, { error: 'too_many_attempts' });
      const body = await readJson(req);
      const user = db.prepare('SELECT * FROM users WHERE username=?').get(body.username);
      if (!user || !verifyPassword(String(body.password || ''), user.password_hash)) {
        loginAttempts.set(ip, { count: state.count + 1, resetAt: Date.now() + 15 * 60_000 });
        audit('login.failed', body.username || 'unknown', { ip });
        return json(res, 401, { error: 'invalid_credentials' });
      }
      loginAttempts.delete(ip);
      const token = newToken();
      const csrf = newToken(24);
      const expiresAt = new Date(Date.now() + 12 * 60 * 60_000).toISOString();
      db.prepare('DELETE FROM sessions WHERE user_id=? OR expires_at<=?').run(user.id, now());
      db.prepare('INSERT INTO sessions(token_hash,user_id,csrf_token,expires_at,created_at) VALUES (?,?,?,?,?)')
        .run(hashToken(token), user.id, csrf, expiresAt, now());
      audit('login.succeeded', user.username, { ip });
      const secure = config.cookieSecure ? '; Secure' : '';
      return json(res, 200, { username: user.username, csrfToken: csrf }, {
        'set-cookie': `bt_session=${encodeURIComponent(token)}; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200${secure}`,
      });
    }

    if (req.method === 'POST' && url.pathname === '/api/auth/logout') {
      const session = requireAdmin(req, res, true);
      if (!session) return;
      db.prepare('DELETE FROM sessions WHERE token_hash=?').run(session.token_hash);
      return json(res, 200, { ok: true }, { 'set-cookie': 'bt_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0' });
    }

    if (req.method === 'POST' && url.pathname === '/api/auth/change-password') {
      const session = requireAdmin(req, res, true);
      if (!session) return;
      const body = await readJson(req);
      const currentPassword = String(body.currentPassword || '');
      const newPassword = String(body.newPassword || '');
      const user = db.prepare('SELECT password_hash FROM users WHERE id=?').get(session.user_id);
      if (!user || !verifyPassword(currentPassword, user.password_hash)) {
        audit('password.change_failed', session.username, { reason: 'current_password_incorrect' });
        return json(res, 401, { error: 'current_password_incorrect' });
      }
      if (newPassword.length < 12 || newPassword.length > 128) {
        return json(res, 400, { error: 'new_password_length_invalid' });
      }
      if (verifyPassword(newPassword, user.password_hash)) {
        return json(res, 400, { error: 'new_password_must_differ' });
      }

      const passwordHash = hashPassword(newPassword);
      db.exec('BEGIN IMMEDIATE');
      try {
        db.prepare('UPDATE users SET password_hash=? WHERE id=?').run(passwordHash, session.user_id);
        db.prepare('DELETE FROM sessions WHERE user_id=?').run(session.user_id);
        audit('password.changed', session.username);
        db.exec('COMMIT');
      } catch (error) {
        db.exec('ROLLBACK');
        throw error;
      }
      const secure = config.cookieSecure ? '; Secure' : '';
      return json(res, 200, { ok: true, reauthenticationRequired: true }, {
        'set-cookie': `bt_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0${secure}`,
      });
    }

    if (req.method === 'GET' && url.pathname === '/api/admin/overview') {
      const session = requireAdmin(req, res);
      if (!session) return;
      const devices = db.prepare(`SELECT id,device_id,name,board,firmware_version,ip_address,last_seen_at,created_at,revoked_at
        FROM devices ORDER BY created_at DESC`).all();
      const firmware = db.prepare(`SELECT id,version,board,channel,file_name,byte_size,sha256,notes,chip,secure_version,
        mandatory,rollout_percent,secure_boot_required,published,created_at
        FROM firmware ORDER BY created_at DESC`).all();
      const providers = db.prepare(`SELECT id,kind,provider,label,enabled,base_url,model,
        CASE WHEN secret_ciphertext IS NULL THEN 0 ELSE 1 END AS has_secret,options_json,updated_at
        FROM provider_configs ORDER BY kind,provider`).all().map((row) => ({ ...row, options: JSON.parse(row.options_json), options_json: undefined }));
      return json(res, 200, { username: session.username, csrfToken: session.csrf_token, devices, firmware, providers });
    }

    if (['GET', 'DELETE'].includes(req.method) &&
        url.pathname === '/api/admin/conversation-memory') {
      const session = requireAdmin(req, res, req.method === 'DELETE');
      if (!session) return;
      const deviceId = url.searchParams.get('deviceId') || '';
      const device = db.prepare('SELECT id FROM devices WHERE device_id=?').get(deviceId);
      if (!device) return json(res, 404, { error: 'device_not_found' });
      if (req.method === 'DELETE') {
        const removed = db.prepare('DELETE FROM conversation_turns WHERE device_id=?')
          .run(device.id).changes;
        audit('conversation_memory.cleared', session.username,
          { deviceId, removed });
        return json(res, 200, { ok: true, removed });
      }
      const summary = db.prepare(`SELECT COUNT(*) AS turns,
        MIN(created_at) AS oldestAt,MAX(created_at) AS newestAt
        FROM conversation_turns WHERE device_id=?`).get(device.id);
      return json(res, 200, { ...summary, maxTurns: 8, retentionDays: 30 });
    }

    if (req.method === 'GET' && url.pathname === '/api/admin/ai-actions') {
      const session = requireAdmin(req, res);
      if (!session) return;
      const limit = Number(url.searchParams.get('limit') || 50);
      if (!Number.isInteger(limit) || limit < 1 || limit > 200) {
        return json(res, 400, { error: 'invalid_limit' });
      }
      const actions = db.prepare(`SELECT a.id,a.trace_id,d.device_id,a.source,
        CASE WHEN e.trace_id IS NOT NULL THEN 'approved' ELSE a.status END AS status,
        a.status AS proposal_status,
        a.action_type,a.direction,a.speed_per_mille,a.duration_ms,a.reason,
        a.diagnostic,CASE WHEN e.result='completed' THEN 1 ELSE 0 END AS executed,
        CASE WHEN e.trace_id IS NULL THEN 'shadow' ELSE 'execute' END AS dispatch_mode,
        e.result AS execution_result,e.reason AS execution_reason,e.reported_at,
        a.created_at FROM ai_action_log a
        JOIN devices d ON d.id=a.device_id
        LEFT JOIN ai_action_execution e ON e.trace_id=a.trace_id
        ORDER BY a.id DESC LIMIT ?`).all(limit);
      return json(res, 200, { actions });
    }

    if (req.method === 'GET' && url.pathname === '/api/admin/mood') {
      const session = requireAdmin(req, res);
      if (!session) return;
      const deviceId = url.searchParams.get('deviceId') || '';
      const device = db.prepare('SELECT id,device_id FROM devices WHERE device_id=?')
        .get(deviceId);
      if (!device) return json(res, 404, { error: 'device_not_found' });
      const mood = readMoodState(device.id);
      const events = db.prepare(`SELECT trace_id,source,trigger_emotion,
        trigger_intensity,valence,arousal,dominance,label,thought_key,created_at
        FROM device_mood_events WHERE device_id=? ORDER BY id DESC LIMIT 30`)
        .all(device.id);
      return json(res, 200, { deviceId: device.device_id, mood, events });
    }

    if (req.method === 'GET' && url.pathname === '/api/admin/ai-explorations') {
      const session = requireAdmin(req, res);
      if (!session) return;
      const limit = Number(url.searchParams.get('limit') || 100);
      if (!Number.isInteger(limit) || limit < 1 || limit > 200) {
        return json(res, 400, { error: 'invalid_limit' });
      }
      const events = db.prepare(`SELECT e.trace_id,d.device_id,e.step_index,e.event,
        e.direction,e.duration_ms,e.light_mode,e.emotion,e.detail,e.created_at
        FROM ai_exploration_events e JOIN devices d ON d.id=e.device_id
        ORDER BY e.id DESC LIMIT ?`).all(limit);
      return json(res, 200, { events });
    }

    if (req.method === 'PUT' && url.pathname === '/api/admin/providers') {
      const session = requireAdmin(req, res, true);
      if (!session) return;
      const body = await readJson(req);
      if (!allowedProviders[body.kind]?.has(body.provider)) return json(res, 400, { error: 'unsupported_provider' });
      if (body.baseUrl) {
        try {
          const parsed = new URL(body.baseUrl);
          const allowedProtocols = body.kind === 'asr' ? ['https:', 'wss:'] : ['https:'];
          if (!allowedProtocols.includes(parsed.protocol) && process.env.NODE_ENV !== 'development') throw new Error();
        } catch { return json(res, 400, { error: 'base_url_must_be_secure' }); }
      }
      const existing = db.prepare('SELECT secret_ciphertext FROM provider_configs WHERE kind=? AND provider=?').get(body.kind, body.provider);
      const submittedCredentials = body.credentials && typeof body.credentials === 'object'
        ? Object.fromEntries(Object.entries(body.credentials).filter(([, value]) => typeof value === 'string' && value.trim()))
        : (body.apiKey ? { apiKey: body.apiKey } : {});
      let mergedCredentials = {};
      if (existing?.secret_ciphertext) {
        try { mergedCredentials = JSON.parse(secrets.decrypt(existing.secret_ciphertext) || '{}'); } catch {}
      }
      const cipher = Object.keys(submittedCredentials).length
        ? secrets.encrypt(JSON.stringify({ ...mergedCredentials, ...submittedCredentials }))
        : existing?.secret_ciphertext || null;
      db.exec('BEGIN IMMEDIATE');
      try {
        if (body.enabled) db.prepare('UPDATE provider_configs SET enabled=0,updated_at=? WHERE kind=?').run(now(), body.kind);
        db.prepare(`INSERT INTO provider_configs(kind,provider,label,enabled,base_url,model,secret_ciphertext,options_json,updated_at)
          VALUES (?,?,?,?,?,?,?,?,?) ON CONFLICT(kind,provider) DO UPDATE SET label=excluded.label,enabled=excluded.enabled,
          base_url=excluded.base_url,model=excluded.model,secret_ciphertext=excluded.secret_ciphertext,
          options_json=excluded.options_json,updated_at=excluded.updated_at`)
          .run(body.kind, body.provider, body.label || body.provider, body.enabled ? 1 : 0, body.baseUrl || null,
            body.model || null, cipher, JSON.stringify(body.options || {}), now());
        db.exec('COMMIT');
      } catch (error) {
        db.exec('ROLLBACK');
        throw error;
      }
      audit('provider.saved', session.username, { kind: body.kind, provider: body.provider, enabled: Boolean(body.enabled) });
      return json(res, 200, { ok: true });
    }

    if (req.method === 'POST' && url.pathname === '/api/admin/devices') {
      const session = requireAdmin(req, res, true);
      if (!session) return;
      const body = await readJson(req);
      if (!isSafeName(body.deviceId) || !isSafeName(body.board)) return json(res, 400, { error: 'invalid_device_or_board' });
      const token = newToken();
      try {
        db.prepare(`INSERT INTO devices(device_id,name,board,token_hash,created_at) VALUES (?,?,?,?,?)`)
          .run(body.deviceId, String(body.name || body.deviceId).slice(0, 80), body.board, hashToken(token), now());
      } catch (error) {
        if (String(error.message).includes('UNIQUE')) return json(res, 409, { error: 'device_exists' });
        throw error;
      }
      audit('device.created', session.username, { deviceId: body.deviceId, board: body.board });
      return json(res, 201, { deviceId: body.deviceId, token });
    }

    const revokeMatch = url.pathname.match(/^\/api\/admin\/devices\/(\d+)\/revoke$/);
    if (req.method === 'POST' && revokeMatch) {
      const session = requireAdmin(req, res, true);
      if (!session) return;
      db.prepare('UPDATE devices SET revoked_at=? WHERE id=?').run(now(), Number(revokeMatch[1]));
      audit('device.revoked', session.username, { id: Number(revokeMatch[1]) });
      return json(res, 200, { ok: true });
    }

    if (req.method === 'PUT' && url.pathname === '/api/admin/firmware') {
      const session = requireAdmin(req, res, true);
      if (!session) return;
      const version = url.searchParams.get('version');
      const board = url.searchParams.get('board');
      const channel = url.searchParams.get('channel') || 'stable';
      const fileName = url.searchParams.get('fileName') || `${board}-${version}.bin`;
      const chip = url.searchParams.get('chip') || 'esp32s3';
      const secureVersion = Number(url.searchParams.get('secureVersion') || 0);
      const rolloutPercent = Number(url.searchParams.get('rolloutPercent') || 100);
      const mandatory = url.searchParams.get('mandatory') === 'true' ? 1 : 0;
      const secureBootRequired = url.searchParams.get('secureBootRequired') === 'true' ? 1 : 0;
      if (![version, board, channel, fileName].every((value) => isSafeName(value, 120)) || extname(fileName).toLowerCase() !== '.bin') {
        return json(res, 400, { error: 'invalid_firmware_metadata' });
      }
      if (!isSafeName(chip) || !Number.isInteger(secureVersion) || secureVersion < 0 ||
          !Number.isInteger(rolloutPercent) || rolloutPercent < 1 || rolloutPercent > 100) {
        return json(res, 400, { error: 'invalid_release_policy' });
      }
      const finalPath = join(config.firmwareDir, `${board}-${channel}-${version}-${newToken(6)}.bin`);
      const tempPath = `${finalPath}.${newToken(8)}.tmp`;
      let size = 0;
      let moved = false;
      const digest = createHash('sha256');
      req.on('data', (chunk) => {
        size += chunk.length;
        digest.update(chunk);
        if (size > config.firmwareMaxBytes) req.destroy(new Error('firmware too large'));
      });
      try {
        await pipeline(req, createWriteStream(tempPath, { flags: 'wx', mode: 0o600 }));
        if (size === 0) throw Object.assign(new Error('empty firmware'), { status: 400 });
        const fd = openSync(tempPath, 'r');
        const magic = Buffer.alloc(1);
        readSync(fd, magic, 0, 1, 0);
        closeSync(fd);
        if (magic[0] !== 0xe9) {
          throw Object.assign(new Error('invalid ESP application image magic'), { status: 400 });
        }
        renameSync(tempPath, finalPath);
        moved = true;
        const result = db.prepare(`INSERT INTO firmware(version,board,channel,file_name,file_path,byte_size,sha256,notes,chip,
          secure_version,mandatory,rollout_percent,secure_boot_required,created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)`)
          .run(version, board, channel, fileName, finalPath, size, digest.digest('hex'), url.searchParams.get('notes') || '',
            chip, secureVersion, mandatory, rolloutPercent, secureBootRequired, now());
        audit('firmware.uploaded', session.username, { id: Number(result.lastInsertRowid), version, board, channel, size });
        return json(res, 201, { id: Number(result.lastInsertRowid), version, board, channel, size });
      } catch (error) {
        rmSync(tempPath, { force: true });
        if (moved) rmSync(finalPath, { force: true });
        if (String(error.message).includes('UNIQUE')) return json(res, 409, { error: 'firmware_version_exists' });
        throw error;
      }
    }

    const publishMatch = url.pathname.match(/^\/api\/admin\/firmware\/(\d+)\/publish$/);
    if (req.method === 'POST' && publishMatch) {
      const session = requireAdmin(req, res, true);
      if (!session) return;
      const row = db.prepare('SELECT * FROM firmware WHERE id=?').get(Number(publishMatch[1]));
      if (!row) return json(res, 404, { error: 'firmware_not_found' });
      db.exec('BEGIN IMMEDIATE');
      try {
        db.prepare('UPDATE firmware SET published=0 WHERE board=? AND channel=?').run(row.board, row.channel);
        db.prepare('UPDATE firmware SET published=1 WHERE id=?').run(row.id);
        db.exec('COMMIT');
      } catch (error) {
        db.exec('ROLLBACK');
        throw error;
      }
      audit('firmware.published', session.username, { id: row.id, version: row.version, board: row.board, channel: row.channel });
      return json(res, 200, { ok: true });
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/heartbeat') {
      const device = requireDevice(req, res);
      if (!device) return;
      const body = await readJson(req);
      db.prepare('UPDATE devices SET firmware_version=?,ip_address=?,last_seen_at=? WHERE id=?')
        .run(String(body.firmwareVersion || device.firmware_version || '').slice(0, 80), req.socket.remoteAddress, now(), device.id);
      return json(res, 200, { serverTime: now(), configRevision: providerRevision(db) });
    }

    if (req.method === 'GET' && url.pathname === '/api/device/v1/config') {
      const device = requireDevice(req, res);
      if (!device) return;
      const active = db.prepare(`SELECT kind,provider,base_url,model,options_json FROM provider_configs
        WHERE enabled=1 ORDER BY updated_at DESC`).all();
      const services = {};
      for (const item of active) if (!services[item.kind]) services[item.kind] = {
        provider: item.provider,
        model: item.model,
        options: JSON.parse(item.options_json),
        proxyUrl: `${publicUrl(req)}/api/device/v1/${item.kind}`,
      };
      return json(res, 200, { deviceId: device.device_id, revision: providerRevision(db), services });
    }

    if (req.method === 'GET' && url.pathname === '/api/device/v1/ota/check') {
      const device = requireDevice(req, res);
      if (!device) return;
      const board = url.searchParams.get('board') || device.board;
      if (board !== device.board) return json(res, 403, { error: 'board_mismatch' });
      const channel = url.searchParams.get('channel') || 'stable';
      const current = url.searchParams.get('version') || device.firmware_version || '0.0.0';
      const chip = url.searchParams.get('chip') || 'esp32s3';
      const deviceSecureVersion = Number(url.searchParams.get('secureVersion') || 0);
      const secureBoot = url.searchParams.get('secureBoot') === '1';
      const release = db.prepare(`SELECT id,version,byte_size,sha256,notes,chip,secure_version,mandatory,rollout_percent,
        secure_boot_required FROM firmware WHERE board=? AND channel=? AND published=1`).get(board, channel);
      const bucket = Number.parseInt(createHash('sha256').update(device.device_id).digest('hex').slice(0, 8), 16) % 100;
      const eligible = release && release.chip === chip && release.secure_version >= deviceSecureVersion &&
        (!release.secure_boot_required || secureBoot) && bucket < release.rollout_percent;
      if (!eligible || compareVersions(release.version, current) <= 0) return json(res, 200, { updateAvailable: false });
      db.prepare(`INSERT INTO ota_events(device_id,firmware_id,event,created_at) VALUES (?,?,?,?)`)
        .run(device.id, release.id, 'available', now());
      return json(res, 200, { updateAvailable: true, version: release.version, size: release.byte_size, sha256: release.sha256,
        notes: release.notes, chip: release.chip, secureVersion: release.secure_version, mandatory: Boolean(release.mandatory),
        url: `${publicUrl(req)}/api/device/v1/firmware/${release.id}` });
    }

    const downloadMatch = url.pathname.match(/^\/api\/device\/v1\/firmware\/(\d+)$/);
    if (req.method === 'GET' && downloadMatch) {
      const device = requireDevice(req, res);
      if (!device) return;
      const row = db.prepare('SELECT * FROM firmware WHERE id=? AND published=1').get(Number(downloadMatch[1]));
      if (!row || row.board !== device.board) return json(res, 404, { error: 'firmware_not_found' });
      const stat = statSync(row.file_path);
      const range = req.headers.range?.match(/^bytes=(\d+)-(\d*)$/);
      const start = range ? Number(range[1]) : 0;
      const end = range && range[2] ? Math.min(Number(range[2]), stat.size - 1) : stat.size - 1;
      if (start < 0 || start > end || start >= stat.size) {
        res.writeHead(416, { 'content-range': `bytes */${stat.size}` });
        return res.end();
      }
      const partial = Boolean(range);
      const headers = { 'content-type': 'application/octet-stream', 'content-length': end - start + 1, 'accept-ranges': 'bytes',
        'content-disposition': `attachment; filename="${row.file_name}"`, 'x-firmware-sha256': row.sha256, 'cache-control': 'private, no-store' };
      if (partial) headers['content-range'] = `bytes ${start}-${end}/${stat.size}`;
      res.writeHead(partial ? 206 : 200, headers);
      return createReadStream(row.file_path, { start, end }).pipe(res);
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/ota/report') {
      const device = requireDevice(req, res);
      if (!device) return;
      const body = await readJson(req);
      const allowedEvents = new Set(['downloaded', 'installed', 'validated', 'rollback', 'failed']);
      if (!allowedEvents.has(body.event)) return json(res, 400, { error: 'invalid_ota_event' });
      const firmware = body.version ? db.prepare('SELECT id FROM firmware WHERE version=? AND board=? ORDER BY created_at DESC LIMIT 1')
        .get(String(body.version), device.board) : null;
      db.prepare('INSERT INTO ota_events(device_id,firmware_id,event,detail,created_at) VALUES (?,?,?,?,?)')
        .run(device.id, firmware?.id || null, body.event, String(body.detail || '').slice(0, 500), now());
      if (body.event === 'validated' && body.version) db.prepare('UPDATE devices SET firmware_version=?,last_seen_at=? WHERE id=?')
        .run(String(body.version).slice(0, 80), now(), device.id);
      return json(res, 200, { ok: true });
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/ai-action/result') {
      const device = requireDevice(req, res);
      if (!device) return;
      const body = await readJson(req, 1024);
      if (typeof body.traceId !== 'string' || !/^[0-9a-f-]{36}$/.test(body.traceId) ||
          !['completed', 'blocked', 'failed'].includes(body.result) ||
          typeof body.direction !== 'string') {
        return json(res, 400, { error: 'action_result_invalid' });
      }
      const row = db.prepare(`SELECT result,direction FROM ai_action_execution
        WHERE trace_id=? AND device_id=?`).get(body.traceId, device.id);
      if (!row) return json(res, 404, { error: 'action_not_authorized' });
      if (row.direction !== body.direction) return json(res, 409, { error: 'action_direction_mismatch' });
      if (row.result && row.result !== body.result) return json(res, 409, { error: 'action_result_conflict' });
      if (!row.result) {
        db.prepare(`UPDATE ai_action_execution SET result=?,reason=?,reported_at=?
          WHERE trace_id=? AND device_id=? AND result IS NULL`).run(body.result,
          String(body.reason || '').slice(0, 80), now(), body.traceId, device.id);
      }
      return json(res, 200, { ok: true });
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/explore/observe') {
      const device = requireDevice(req, res);
      if (!device) return;
      const traceId = url.searchParams.get('traceId') || '';
      const step = Number(url.searchParams.get('step'));
      if (!/^[0-9a-f-]{36}$/.test(traceId) || !Number.isInteger(step)) {
        return json(res, 400, { error: 'exploration_request_invalid' });
      }
      const state = explorations.get(traceId);
      if (!state || state.deviceId !== device.id || Date.now() > state.expiresAt ||
          state.finished || state.busy || state.pending || step !== state.steps ||
          step >= EXPLORATION_MAX_STEPS) {
        return json(res, 409, { error: 'exploration_not_ready' });
      }
      if (req.headers['content-type'] !== 'image/jpeg') {
        return json(res, 415, { error: 'jpeg_required' });
      }
      state.busy = true;
      try {
        const jpeg = await readBody(req, 160 * 1024);
        if (jpeg.length < 128 || jpeg.length > 160 * 1024 ||
            jpeg[0] !== 0xff || jpeg[1] !== 0xd8 ||
            jpeg.at(-2) !== 0xff || jpeg.at(-1) !== 0xd9) {
          return json(res, 400, { error: 'jpeg_invalid' });
        }
        logExploration(device.id, traceId, step, 'observed', { detail: `jpeg_bytes=${jpeg.length}` });
        const asr = activeProvider('asr');
        if (asr?.provider !== 'qwen') return json(res, 503, { error: 'vision_credentials_unavailable' });
        const credentials = providerCredentials(asr);
        const remainingMs = EXPLORATION_MAX_TOTAL_MS - state.totalMs;
        const upstream = await requestChatCompletion({ selected: {
          provider: 'qwen', model: 'qwen3-vl-plus', base_url: null,
          options_json: JSON.stringify({ thinkingMode: 'off' }),
        }, credentials, request: {
          messages: [{ role: 'system', content: visionPrompt },
            { role: 'user', content: [
              { type: 'text', text: `第 ${step + 1} 次观察；还可行走 ${remainingMs} 毫秒。请只看照片并决定下一步；不确定就停止。` },
              { type: 'image_url', image_url: { url: `data:image/jpeg;base64,${jpeg.toString('base64')}` } },
            ] }], stream: false, temperature: 0, max_tokens: 180,
        } });
        if (Date.now() > state.expiresAt || state.finished) {
          return json(res, 409, { error: 'exploration_expired' });
        }
        if (!upstream.ok) {
          logExploration(device.id, traceId, step, 'failed', { detail: `vision_http_${upstream.status}` });
          return json(res, 502, { error: 'vision_upstream_rejected' });
        }
        const body = await upstream.json();
        const proposal = parseVisionStep(body?.choices?.[0]?.message?.content,
          { usedSteps: state.steps, usedMs: state.totalMs });
        // Bad or over-budget model output is a STOP, never a guessed move.
        const decision = proposal || { move: 'stop', durationMs: 0,
          lightMode: 'unchanged', emotion: 'worried', intensity: 55,
          description: '视觉判断不可靠，已停止' };
        if (decision.move === 'stop') {
          state.finished = true;
          logExploration(device.id, traceId, step, 'stopped', {
            lightMode: decision.lightMode, emotion: decision.emotion,
            detail: decision.description });
        } else {
          state.pending = decision;
          logExploration(device.id, traceId, step, 'planned', {
            direction: decision.move, durationMs: decision.durationMs,
            lightMode: decision.lightMode, emotion: decision.emotion,
            detail: decision.description });
        }
        return json(res, 200, { traceId, step, ...decision });
      } finally {
        state.busy = false;
      }
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/vision/describe') {
      const device = requireDevice(req, res);
      if (!device) return;
      const traceId = url.searchParams.get('traceId') || '';
      const state = visionSessions.get(traceId);
      if (!/^[0-9a-f-]{36}$/.test(traceId) || !state || state.deviceId !== device.id ||
          Date.now() > state.expiresAt || state.busy) {
        return json(res, 409, { error: 'vision_turn_not_ready' });
      }
      if (req.headers['content-type'] !== 'image/jpeg') {
        return json(res, 415, { error: 'jpeg_required' });
      }
      state.busy = true;
      try {
        const jpeg = await readBody(req, 160 * 1024);
        if (jpeg.length < 128 || jpeg[0] !== 0xff || jpeg[1] !== 0xd8 ||
            jpeg.at(-2) !== 0xff || jpeg.at(-1) !== 0xd9) {
          return json(res, 400, { error: 'jpeg_invalid' });
        }
        logCamera(device.id, traceId, 'received', `jpeg_bytes=${jpeg.length}`);
        const visionProvider = activeProvider('asr');
        const tts = activeProvider('tts');
        if (visionProvider?.provider !== 'qwen' || tts?.provider !== 'volcengine') {
          return json(res, 503, { error: 'vision_provider_unavailable' });
        }
        const previous = readConversationMemory(db, secrets, device.id);
        const currentMood = readMoodState(device.id);
        const messages = conversationMessages(state.userText, previous,
          state.personality || {}, currentMood);
        messages.splice(1, 0, { role: 'system', content: visionAnswerPrompt });
        messages[messages.length - 1].content = [
          { type: 'image_url', image_url: { url: `data:image/jpeg;base64,${jpeg.toString('base64')}` } },
          { type: 'text', text: `请只回答孩子刚说的话：“${state.userText}”。画面仅供必要时参考；若问题与眼前环境无关，不要提及画面。` },
        ];
        const upstream = await requestChatCompletion({ selected: {
          provider: 'qwen', model: 'qwen3-vl-plus', base_url: null,
          options_json: JSON.stringify({ thinkingMode: 'off' }),
        }, credentials: providerCredentials(visionProvider), request: {
          messages, stream: false, temperature: 0.5, max_tokens: 400,
        } });
        if (!upstream.ok) return json(res, 502, { error: 'vision_upstream_rejected' });
        const visionBody = await upstream.json();
        const reply = parseConversationReply(visionBody?.choices?.[0]?.message?.content);
        if (!reply) {
          return json(res, 502, { error: 'vision_reply_invalid' });
        }
        const nextMood = advanceMood(currentMood,
          reply.speechSegments.at(-1).emotion,
          reply.speechSegments.at(-1).intensity, reply.thought);
        /* A vision model can describe a scene, but cannot grant movement or
         * recursively request another photo. Those fields are ignored. */
        const chunks = [];
        let total = 0;
        for (const segment of reply.speechSegments) {
          const synthesized = await synthesizeWithVolcengine({ selected: tts,
            credentials: providerCredentials(tts), text: segment.text,
            deviceId: device.device_id });
          if (synthesized.format !== 'pcm' || synthesized.sampleRate !== 16000) {
            return json(res, 503, { error: 'vision_tts_format_invalid' });
          }
          for await (const chunk of synthesized.audio) {
            total += chunk.length;
            if (total > 2 * 1024 * 1024) return json(res, 502, { error: 'vision_tts_too_long' });
            chunks.push(chunk);
          }
        }
        if (total < 2 || total % 2 !== 0) return json(res, 502, { error: 'vision_tts_invalid' });
        rememberConversation(device.id, state.userText, reply.text);
        rememberMood(device.id, traceId, 'vision',
          reply.speechSegments.at(-1).emotion,
          reply.speechSegments.at(-1).intensity, nextMood);
        logCamera(device.id, traceId, 'answered', `pcm_bytes=${total}`);
        res.writeHead(200, { 'content-type': 'audio/pcm', 'cache-control': 'no-store',
          'x-audio-format': 'pcm', 'x-audio-sample-rate': '16000',
          'x-ai-emotion': reply.emotion, 'x-ai-intensity': String(reply.intensity),
          ...moodHeaders(nextMood),
          'content-length': total });
        return res.end(Buffer.concat(chunks, total));
      } catch (error) {
        logCamera(device.id, traceId, 'failed', String(error.code || error.message || 'vision_failed'));
        if (res.headersSent) return res.destroy(error);
        return json(res, error.status || 502, { error: error.code || 'vision_failed' });
      } finally {
        visionSessions.delete(traceId);
      }
    }

    if (req.method === 'GET' && url.pathname === '/api/device/v1/drawing') {
      const device = requireDevice(req, res);
      if (!device) return;
      const traceId = url.searchParams.get('traceId') || '';
      const state = drawingSessions.get(traceId);
      if (!/^[0-9a-f-]{36}$/.test(traceId) || !state ||
          state.deviceId !== device.id || Date.now() > state.expiresAt) {
        return json(res, 409, { error: 'drawing_not_ready' });
      }
      drawingSessions.delete(traceId);
      return res.writeHead(200, { 'content-type': 'image/jpeg',
        'content-length': state.drawing.jpeg.length,
        'x-drawing-duration-ms': String(state.drawing.durationMs),
        'cache-control': 'no-store', 'x-content-type-options': 'nosniff' })
        .end(state.drawing.jpeg);
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/explore/result') {
      const device = requireDevice(req, res);
      if (!device) return;
      const body = await readJson(req, 1024);
      const state = explorations.get(body.traceId);
      if (!state || state.deviceId !== device.id || Date.now() > state.expiresAt ||
          state.finished || !state.pending || body.step !== state.steps ||
          body.direction !== state.pending.move ||
          !['completed', 'blocked', 'failed'].includes(body.result)) {
        return json(res, 409, { error: 'exploration_result_invalid' });
      }
      logExploration(device.id, body.traceId, body.step, body.result, {
        direction: body.direction, durationMs: state.pending.durationMs,
        detail: String(body.reason || '').slice(0, 80) });
      if (body.result === 'completed') {
        state.totalMs += state.pending.durationMs;
        ++state.steps;
        state.pending = null;
        if (state.steps >= EXPLORATION_MAX_STEPS ||
            EXPLORATION_MAX_TOTAL_MS - state.totalMs < 2000) state.finished = true;
      } else {
        state.finished = true;
        state.pending = null;
      }
      return json(res, 200, { ok: true, finished: state.finished,
        usedSteps: state.steps, usedMs: state.totalMs });
    }

    if (req.method === 'POST' && ['/api/device/v1/conversation', '/api/device/v1/conversation/stream'].includes(url.pathname)) {
      const streaming = url.pathname.endsWith('/stream');
      const device = requireDevice(req, res);
      if (!device) return;
      if (conversationBusy.has(device.id)) return json(res, 409, { error: 'conversation_busy' });
      const asr = activeProvider('asr');
      const llm = activeProvider('llm');
      const tts = activeProvider('tts');
      if (!asr || !llm || !tts) return json(res, 503, { error: 'conversation_provider_not_configured' });
      if (asr.provider !== 'qwen' || tts.provider !== 'volcengine') {
        return json(res, 501, { error: 'conversation_provider_not_implemented' });
      }
      if (url.searchParams.get('format') !== 'pcm' || url.searchParams.get('sampleRate') !== '16000') {
        return json(res, 400, { error: 'conversation_audio_format_invalid' });
      }
      const touch = url.searchParams.get('touch');
      const diagnostic = url.searchParams.get('diagnostic') === '1';
      const wire = url.searchParams.get('wire');
      if (wire !== null && (wire !== 'framed-v1' || !streaming)) {
        return json(res, 400, { error: 'conversation_wire_invalid' });
      }
      const framed = wire === 'framed-v1';
      if (streaming && touch !== null) return json(res, 400, { error: 'conversation_stream_touch_invalid' });
      if (touch !== null && touch !== 'left' && touch !== 'right') {
        return json(res, 400, { error: 'conversation_touch_invalid' });
      }
      const personality = Object.fromEntries(['curiosity', 'warmth', 'talkativeness']
        .map((key) => [key, url.searchParams.get(key)]));
      conversationBusy.add(device.id);
      const startedAt = performance.now();
      const metrics = { event: 'conversation.metrics', traceId: randomUUID(), device: device.id,
        kind: touch === null ? 'speech' : 'touch', diagnostic, streaming, inputBytes: 0 };
      let logged = false;
      const logMetrics = (event) => {
        if (logged) return;
        logged = true;
        metrics.httpStatus = res.statusCode;
        metrics.responseEvent = event;
        metrics.totalMs = Math.round(performance.now() - startedAt);
        metrics.loggedAt = now();
        console.info(JSON.stringify(metrics));
        try {
          db.prepare('INSERT INTO conversation_metrics(trace_id,device_id,metrics_json,created_at) VALUES (?,?,?,?)')
            .run(metrics.traceId, device.id, JSON.stringify(metrics), metrics.loggedAt);
          db.exec('DELETE FROM conversation_metrics WHERE id NOT IN (SELECT id FROM conversation_metrics ORDER BY id DESC LIMIT 1000)');
        } catch (error) {
          console.error(`conversation metrics persistence failed: ${error.message}`);
        }
      };
      res.once('finish', () => logMetrics('finish'));
      res.once('close', () => logMetrics('close'));
      try {
        let audio = null;
        if (!streaming) {
          audio = await readBody(req, config.speechMaxBytes || 4 * 1024 * 1024);
          metrics.inputBytes = audio.length;
          if (touch === null && (audio.length < 2 || audio.length % 2 !== 0)) {
            return json(res, 400, { error: 'conversation_audio_invalid' });
          }
          if (touch !== null && audio.length !== 0) return json(res, 400, { error: 'conversation_touch_audio_invalid' });
        }
        async function* streamedAudio() {
          const limit = config.speechMaxBytes || 4 * 1024 * 1024;
          let firstAt = 0;
          for await (const chunk of req) {
            if (!firstAt) firstAt = performance.now();
            metrics.inputBytes += chunk.length;
            if (metrics.inputBytes > limit) {
              throw Object.assign(new Error('conversation_audio_too_large'), { status: 413, code: 'conversation_audio_too_large' });
            }
            yield chunk;
          }
          if (req.aborted) {
            throw Object.assign(new Error('conversation_upload_aborted'), { status: 400, code: 'conversation_upload_aborted' });
          }
          metrics.uploadMs = Math.round(performance.now() - (firstAt || startedAt));
        }
        const asrStartedAt = performance.now();
        const recognized = touch === null
          ? await (streaming
            ? recognizeStreamWithQwen({ selected: asr, credentials: providerCredentials(asr),
              audioChunks: streamedAudio(), format: 'pcm', sampleRate: 16000,
              webSocketFactory: config.webSocketFactory })
            : recognizeWithQwen({ selected: asr, credentials: providerCredentials(asr),
              audio, format: 'pcm', sampleRate: 16000, webSocketFactory: config.webSocketFactory,
              frameDelayMs: config.asrFrameDelayMs ?? 100 }))
          : { text: `[触摸事件] 用户刚刚从你的${touch === 'left' ? '左侧' : '右侧'}轻轻摸了你一下。` +
            '你是从待机睡觉中被轻轻摸醒的。结合当前长期心情，给出一句自然、简短、可被TTS朗读的即时反应，' +
            '并为这次触摸选择匹配的瞬时表情和新的睡眠气泡想法。不要反问，不要解释这是系统事件，' +
            '不要每次都使用同一句话，也不要输出行走、摄像头或绘画动作。' };
        metrics.asrMs = Math.round(performance.now() - asrStartedAt);
        const userText = typeof recognized.text === 'string' ? recognized.text.trim() : '';
        if (!userText) return json(res, 422, { error: 'conversation_speech_not_recognized' });
        const sleepRequested = touch === null && explicitSleepIntent(userText);
        metrics.sessionSleep = sleepRequested;
        /* The head advertises vision support. ASR text, not the legacy
         * vision=always parameter, decides whether a photo is needed. Direct
         * controls never wait for a photo. */
        const visualContextTurn = streaming &&
          url.searchParams.get('vision') === 'always' && touch === null &&
          !diagnostic && !sleepRequested && !explicitDriveIntent(userText) &&
          !explicitExploreIntent(userText) && !explicitDrawIntent(userText) &&
          explicitLookIntent(userText);
        if (visualContextTurn) {
          for (const [key, value] of visionSessions) {
            if (Date.now() > value.expiresAt) visionSessions.delete(key);
          }
          if (visionSessions.size < 100) {
            visionSessions.set(metrics.traceId, { deviceId: device.id, userText,
              personality, busy: false, expiresAt: Date.now() + 120_000 });
            logCamera(device.id, metrics.traceId, 'requested');
            metrics.actionType = 'look';
            metrics.actionStatus = 'approved';
            metrics.visualContext = true;
            return res.writeHead(200, { 'content-type': 'audio/pcm',
              'content-length': '0', 'x-audio-format': 'pcm',
              'x-audio-sample-rate': '16000', 'x-ai-action-version': '5',
              'x-ai-action-mode': 'execute', 'x-ai-action-status': 'approved',
              'x-ai-action-type': 'look', 'x-turn-trace-id': metrics.traceId,
              'cache-control': 'no-store' }).end();
          }
          metrics.visualFallback = 'session_capacity';
        }
        const previous = diagnostic ? [] : readConversationMemory(db, secrets, device.id);
        const currentMood = readMoodState(device.id);
        const llmStartedAt = performance.now();
        const upstream = await requestChatCompletion({ selected: llm, credentials: providerCredentials(llm),
          request: { messages: conversationMessages(userText, previous, personality,
            currentMood), stream: false,
            temperature: 0.7, max_tokens: explicitDrawIntent(userText) ? 1000 : 400 } });
        if (!upstream.ok) return json(res, 502, { error: 'conversation_llm_rejected' });
        const llmBody = await upstream.json();
        let reply = parseConversationReply(llmBody?.choices?.[0]?.message?.content);
        if (!reply) return json(res, 502, { error: 'conversation_llm_response_invalid' });
        const explicitDrawingRequested = touch === null && explicitDrawIntent(userText);
        const autonomousDrawingRequested = touch === null && !diagnostic &&
          !explicitDrawingRequested && !sleepRequested && !explicitDriveIntent(userText) &&
          !explicitExploreIntent(userText) && !reply.cameraRequested &&
          reply.action.status !== 'shadow' && reply.drawing !== null;
        const drawingRequested = explicitDrawingRequested || autonomousDrawingRequested;
        const drawingSubject = autonomousDrawingRequested ? reply.drawing.subject : userText;
        let drawingCandidate = null;
        if (touch === null && !diagnostic && drawingRequested &&
            !explicitDriveIntent(userText)) {
          try {
            const drawingUpstream = await requestChatCompletion({ selected: llm,
              credentials: providerCredentials(llm), request: {
                messages: drawingMessages(drawingSubject), stream: false,
                temperature: 0.35, max_tokens: 3000,
              } });
            if (drawingUpstream.ok) {
              const drawingBody = await drawingUpstream.json();
              const svgDrawing = parseDrawingProposal(
                drawingBody?.choices?.[0]?.message?.content);
              drawingCandidate = svgDrawing ? await rasterizeDrawing(svgDrawing) : null;
              metrics.drawingSource = drawingCandidate ? 'curated_svg_raster' : 'invalid';
              metrics.drawingDecision = autonomousDrawingRequested ? 'model' : 'asr_rule';
              if (drawingCandidate) metrics.drawingBytes = drawingCandidate.jpeg.length;
            }
          } catch (error) {
            metrics.drawingError = String(error.code || error.message || 'generation_failed').slice(0, 80);
          }
        }
        if (explicitDrawingRequested && !diagnostic && !drawingCandidate) {
          reply = parseConversationReply(JSON.stringify({ reply: '这次没画出来，我们再试一次吧。',
            emotion: 'confused', intensity: 55, action: null, camera: null }));
        }
        const moodTrigger = reply.speechSegments.at(-1);
        const nextMood = advanceMood(currentMood, moodTrigger.emotion,
          moodTrigger.intensity, reply.thought);
        metrics.llmMs = Math.round(performance.now() - llmStartedAt);
        const speechSegments = reply.speechSegments;
        metrics.modelFaceCueCount = speechSegments.length;
        metrics.modelFaceCueSource = reply.speechSegmentSource;
        // The model may choose motion autonomously, but only inside the
        // deterministic physical envelope enforced by schema validation,
        // bounded duration/speed, STOP preemption, and mode ownership.
        const explicitIntent = touch === null ? explicitDriveIntent(userText) : null;
        const exploreRequested = touch === null && !diagnostic && explicitExploreIntent(userText);
        const action = exploreRequested && reply.action.status === 'shadow'
          ? { status: 'rejected', type: 'none', reason: 'exploration_uses_visual_plan' }
          : touch !== null && reply.action.status === 'shadow'
          ? { status: 'rejected', type: 'none', reason: 'touch_cannot_drive' }
          : explicitIntent
            ? { status: 'shadow', type: 'drive', direction: explicitIntent.direction,
              speedPerMille: reply.action.status === 'shadow' &&
                reply.action.direction === explicitIntent.direction
                ? reply.action.speedPerMille : 900,
              durationMs: explicitIntent.durationMs ??
                (reply.action.status === 'shadow' &&
                 reply.action.direction === explicitIntent.direction
                  ? reply.action.durationMs : 2000),
              lightMode: reply.action.status === 'shadow' &&
                reply.action.direction === explicitIntent.direction
                ? reply.action.lightMode : 'unchanged', reason: '' }
            : reply.action.status === 'shadow' &&
                (sleepRequested || drawingRequested ||
                 reply.cameraRequested)
              ? { status: 'rejected', type: 'none', reason: 'mode_cannot_drive' }
              : reply.action;
        let dispatchMode = action.status === 'shadow' && !diagnostic ? 'execute' : 'shadow';
        metrics.actionStatus = action.status;
        metrics.actionType = action.type;
        if (action.type === 'drive') metrics.actionDirection = action.direction;
        try {
          db.exec('BEGIN IMMEDIATE');
          db.prepare(`INSERT INTO ai_action_log(trace_id,device_id,source,status,
            action_type,direction,speed_per_mille,duration_ms,reason,diagnostic,created_at)
            VALUES (?,?,?,?,?,?,?,?,?,?,?)`).run(metrics.traceId, device.id,
            touch === null ? 'speech' : 'touch', action.status, action.type,
            action.direction || null, action.speedPerMille || null,
            action.durationMs || null, action.reason, diagnostic ? 1 : 0, now());
          if (dispatchMode === 'execute') {
            db.prepare(`INSERT INTO ai_action_execution(trace_id,device_id,direction,authorized_at)
              VALUES (?,?,?,?)`).run(metrics.traceId, device.id, action.direction, now());
          }
          db.exec('COMMIT');
        } catch (error) {
          if (db.isTransaction) db.exec('ROLLBACK');
          dispatchMode = 'shadow';
          metrics.actionLogError = true;
          console.error(`AI action shadow log failed: ${error.message}`);
        }
        let exploreAuthorized = false;
        if (exploreRequested && !metrics.actionLogError) {
          try {
            if (explorations.size >= 100) {
              for (const [key, value] of explorations) {
                if (value.finished || Date.now() > value.expiresAt) explorations.delete(key);
              }
            }
            if (explorations.size < 100) {
              explorations.set(metrics.traceId, { deviceId: device.id,
                steps: 0, totalMs: 0, pending: null, busy: false,
                finished: false, expiresAt: Date.now() + 120_000 });
              logExploration(device.id, metrics.traceId, 0, 'authorized');
              exploreAuthorized = true;
              metrics.actionType = 'explore';
              metrics.actionStatus = 'approved';
            }
          } catch (error) {
            explorations.delete(metrics.traceId);
            console.error(`AI exploration authorization failed: ${error.message}`);
          }
        }
        const lookRequested = touch === null && !diagnostic && !sleepRequested &&
          !exploreRequested && !explicitIntent && !drawingRequested &&
          (reply.cameraRequested || explicitLookIntent(userText));
        if (lookRequested && !metrics.actionLogError) {
          for (const [key, value] of visionSessions) {
            if (Date.now() > value.expiresAt) visionSessions.delete(key);
          }
          if (visionSessions.size < 100) {
            visionSessions.set(metrics.traceId, { deviceId: device.id, userText,
              personality,
              busy: false, expiresAt: Date.now() + 120_000 });
            logCamera(device.id, metrics.traceId, 'requested');
            metrics.visualDecision = reply.cameraRequested ? 'model' : 'asr_rule';
            metrics.actionType = 'look';
            metrics.actionStatus = 'approved';
            return res.writeHead(200, { 'content-type': 'audio/pcm',
              'content-length': '0', 'x-audio-format': 'pcm',
              'x-audio-sample-rate': '16000', 'x-ai-action-version': '5',
              'x-ai-action-mode': 'execute', 'x-ai-action-status': 'approved',
              'x-ai-action-type': 'look', 'x-turn-trace-id': metrics.traceId,
              'cache-control': 'no-store' }).end();
          }
        }
        let drawAuthorized = false;
        if (!lookRequested && touch === null && !diagnostic && !sleepRequested &&
            !exploreRequested && !explicitIntent && drawingRequested &&
            drawingCandidate && !metrics.actionLogError) {
          for (const [key, value] of drawingSessions) {
            if (Date.now() > value.expiresAt) drawingSessions.delete(key);
          }
          if (drawingSessions.size < 100) {
            drawingSessions.set(metrics.traceId, { deviceId: device.id,
              drawing: drawingCandidate, expiresAt: Date.now() + 120_000 });
            drawAuthorized = true;
            metrics.actionType = 'draw';
            metrics.actionStatus = 'approved';
          }
        }
        const commonAudioHeaders = {
          'content-type': 'audio/pcm', 'cache-control': 'no-store',
          'x-audio-format': 'pcm', 'x-audio-sample-rate': '16000',
          'x-ai-emotion': speechSegments[0].emotion,
          'x-ai-intensity': String(speechSegments[0].intensity),
          ...moodHeaders(nextMood),
          'x-ai-session': sleepRequested ? 'sleep' : 'continue',
          'x-ai-action-version': drawAuthorized ? '6' : exploreAuthorized ? '3' : '4',
          'x-ai-action-mode': drawAuthorized || exploreAuthorized ? 'execute' : dispatchMode,
          'x-ai-action-status': drawAuthorized || exploreAuthorized || dispatchMode === 'execute' ? 'approved' : action.status,
          'x-ai-action-type': drawAuthorized ? 'draw' : exploreAuthorized ? 'explore' : action.type,
          ...(!exploreAuthorized && action.type === 'drive' && dispatchMode === 'execute' ? {
            'x-ai-action-direction': action.direction,
            'x-ai-action-speed-per-mille': String(action.speedPerMille),
            'x-ai-action-duration-ms': String(action.durationMs),
            'x-ai-action-light-mode': action.lightMode || 'unchanged',
          } : {}),
          'x-turn-trace-id': metrics.traceId,
          'x-content-type-options': 'nosniff',
        };
        const ttsStartedAt = performance.now();
        if (framed) {
          metrics.ttsMode = 'framed-v1';
          metrics.ttsChunks = 0;
          metrics.ttsMaxGapMs = 0;
          metrics.ttsGapsOver100Ms = 0;
          metrics.ttsGapsOver500Ms = 0;
          let audioBytes = 0;
          let firstChunkAt = 0;
          let previousChunkAt = 0;
          for (let index = 0; index < speechSegments.length; ++index) {
            const segment = speechSegments[index];
            const synthesized = await synthesizeWithVolcengine({ selected: tts,
              credentials: providerCredentials(tts), text: segment.text,
              deviceId: device.device_id });
            if (synthesized.format !== 'pcm' || synthesized.sampleRate !== 16000) {
              throw Object.assign(new Error('conversation_tts_must_be_pcm16k'),
                { status: 503, code: 'conversation_tts_must_be_pcm16k' });
            }
            const segmentStartBytes = audioBytes;
            let cueSent = index === 0;
            for await (const chunk of synthesized.audio) {
              if (chunk.length === 0) continue;
              const chunkAt = performance.now();
              if (!firstChunkAt) {
                firstChunkAt = chunkAt;
                metrics.ttsFirstChunkMs = Math.round(chunkAt - ttsStartedAt);
              } else {
                const gapMs = chunkAt - previousChunkAt;
                metrics.ttsMaxGapMs = Math.max(metrics.ttsMaxGapMs, Math.round(gapMs));
                if (gapMs > 100) ++metrics.ttsGapsOver100Ms;
                if (gapMs > 500) ++metrics.ttsGapsOver500Ms;
              }
              previousChunkAt = chunkAt;
              if (audioBytes + chunk.length > 2 * 1024 * 1024) {
                throw Object.assign(new Error('conversation_tts_too_long'),
                  { status: 502, code: 'conversation_tts_too_long' });
              }
              if (!res.headersSent) {
                res.writeHead(200, { ...commonAudioHeaders,
                  'x-ai-face-cues': `0:${speechSegments[0].emotion}:${speechSegments[0].intensity}`,
                  'x-babytank-audio-framing': 'v1',
                  'server-timing': `asr;dur=${metrics.asrMs}, llm;dur=${metrics.llmMs}` });
                metrics.preSendMs = Math.round(performance.now() - startedAt);
              }
              if (!cueSent) {
                await writePcmFrame(res, 0x45,
                  Buffer.from(`${segment.emotion}:${segment.intensity}`, 'ascii'));
                cueSent = true;
              }
              for (let offset = 0; offset < chunk.length; offset += PCM_FRAME_BYTES) {
                await writePcmFrame(res, 0x50,
                  chunk.subarray(offset, offset + PCM_FRAME_BYTES));
              }
              audioBytes += chunk.length;
              ++metrics.ttsChunks;
            }
            if (audioBytes === segmentStartBytes || (audioBytes - segmentStartBytes) % 2 !== 0) {
              throw Object.assign(new Error('conversation_tts_invalid_pcm'),
                { status: 502, code: 'conversation_tts_invalid_pcm' });
            }
          }
          if (audioBytes === 0) {
            throw Object.assign(new Error('conversation_tts_invalid_pcm'),
              { status: 502, code: 'conversation_tts_invalid_pcm' });
          }
          metrics.ttsMs = Math.round(performance.now() - ttsStartedAt);
          metrics.ttsStreamMs = Math.round(previousChunkAt - firstChunkAt);
          metrics.outputBytes = audioBytes;
          metrics.pcmDurationMs = Math.round(audioBytes / 32);
          metrics.faceCueCount = speechSegments.length;
          if (!diagnostic) {
            rememberConversation(device.id, userText, reply.text);
            rememberMood(device.id, metrics.traceId,
              touch === null ? 'speech' : 'touch', moodTrigger.emotion,
              moodTrigger.intensity, nextMood);
          }
          return res.end();
        }
        // Upstream PCM currently arrives slower than real-time playback. Cache
        // the answer in cloud RAM. Synthesize each semantic sentence separately
        // so cue offsets are exact PCM sample boundaries, not guessed from text.
        const audioChunks = [];
        const faceCues = [];
        let audioBytes = 0;
        metrics.ttsChunks = 0;
        metrics.ttsMaxGapMs = 0;
        metrics.ttsGapsOver100Ms = 0;
        metrics.ttsGapsOver500Ms = 0;
        const synthesizedSegments = await Promise.all(speechSegments.map(async (segment) => {
          const synthesized = await synthesizeWithVolcengine({ selected: tts,
            credentials: providerCredentials(tts), text: segment.text,
            deviceId: device.device_id });
          if (synthesized.format !== 'pcm' || synthesized.sampleRate !== 16000) {
            throw Object.assign(new Error('conversation_tts_must_be_pcm16k'),
              { status: 503, code: 'conversation_tts_must_be_pcm16k' });
          }
          const chunks = [];
          let bytes = 0;
          let firstChunkAt = 0;
          let previousChunkAt = 0;
          let lastChunkAt = 0;
          let maxGapMs = 0;
          let gapsOver100Ms = 0;
          let gapsOver500Ms = 0;
          for await (const chunk of synthesized.audio) {
            const chunkAt = performance.now();
            if (!firstChunkAt) firstChunkAt = chunkAt;
            else {
              const gapMs = chunkAt - previousChunkAt;
              maxGapMs = Math.max(maxGapMs, Math.round(gapMs));
              if (gapMs > 100) ++gapsOver100Ms;
              if (gapMs > 500) ++gapsOver500Ms;
            }
            previousChunkAt = chunkAt;
            lastChunkAt = chunkAt;
            bytes += chunk.length;
            chunks.push(chunk);
          }
          if (bytes === 0 || bytes % 2 !== 0) {
            throw Object.assign(new Error('conversation_tts_invalid_pcm'),
              { status: 502, code: 'conversation_tts_invalid_pcm' });
          }
          return { segment, chunks, bytes, firstChunkAt, lastChunkAt,
            chunkCount: chunks.length, maxGapMs, gapsOver100Ms, gapsOver500Ms };
        }));
        let firstChunkAt = 0;
        let lastChunkAt = 0;
        for (const synthesized of synthesizedSegments) {
          faceCues.push(`${audioBytes / 2}:${synthesized.segment.emotion}:${synthesized.segment.intensity}`);
          audioBytes += synthesized.bytes;
          if (audioBytes > 2 * 1024 * 1024) {
            return json(res, 502, { error: 'conversation_tts_too_long' });
          }
          audioChunks.push(...synthesized.chunks);
          metrics.ttsChunks += synthesized.chunkCount;
          metrics.ttsMaxGapMs = Math.max(metrics.ttsMaxGapMs, synthesized.maxGapMs);
          metrics.ttsGapsOver100Ms += synthesized.gapsOver100Ms;
          metrics.ttsGapsOver500Ms += synthesized.gapsOver500Ms;
          if (!firstChunkAt || synthesized.firstChunkAt < firstChunkAt) {
            firstChunkAt = synthesized.firstChunkAt;
          }
          if (synthesized.lastChunkAt > lastChunkAt) lastChunkAt = synthesized.lastChunkAt;
        }
        if (audioBytes === 0 || audioBytes % 2 !== 0) {
          return json(res, 502, { error: 'conversation_tts_invalid_pcm' });
        }
        metrics.ttsMs = Math.round(performance.now() - ttsStartedAt);
        metrics.ttsStreamMs = Math.round(lastChunkAt - firstChunkAt);
        metrics.outputBytes = audioBytes;
        metrics.pcmDurationMs = Math.round(audioBytes / 32);
        metrics.faceCueCount = faceCues.length;
        metrics.preSendMs = Math.round(performance.now() - startedAt);
        const answerAudio = Buffer.concat(audioChunks, audioBytes);
        if (!diagnostic) {
          rememberConversation(device.id, userText, reply.text);
          rememberMood(device.id, metrics.traceId,
            touch === null ? 'speech' : 'touch', moodTrigger.emotion,
            moodTrigger.intensity, nextMood);
        }
        res.writeHead(200, { ...commonAudioHeaders,
          'x-ai-face-cues': faceCues.join(','),
          'x-turn-tts-max-gap-ms': String(metrics.ttsMaxGapMs),
          'x-turn-tts-stream-ms': String(metrics.ttsStreamMs),
          'x-turn-pcm-duration-ms': String(metrics.pcmDurationMs),
          'server-timing': `asr;dur=${metrics.asrMs}, llm;dur=${metrics.llmMs}, tts;dur=${metrics.ttsMs}`,
          'content-length': answerAudio.length });
        return res.end(answerAudio);
      } catch (error) {
        metrics.error = error.code || error.message || 'conversation_failed';
        if (res.headersSent) return res.destroy(error);
        return json(res, error.status || 502, { error: error.code || error.message || 'conversation_failed' });
      } finally {
        conversationBusy.delete(device.id);
      }
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/llm') {
      const device = requireDevice(req, res);
      if (!device) return;
      const selected = activeProvider('llm');
      if (!selected) return json(res, 503, { error: 'llm_not_configured' });
      const body = await readJson(req, 256 * 1024);
      if (!validateChatMessages(body.messages)) return json(res, 400, { error: 'messages_invalid' });
      let upstream;
      try {
        upstream = await requestChatCompletion({ selected, credentials: providerCredentials(selected), request: body });
      } catch (error) {
        return json(res, error.status || 502, { error: error.code || error.message || 'llm_upstream_failed' });
      }
      const contentType = upstream.headers.get('content-type') || (body.stream ? 'text/event-stream' : 'application/json');
      res.writeHead(upstream.status, { 'content-type': contentType, 'cache-control': 'no-store', 'x-content-type-options': 'nosniff' });
      if (!upstream.body) return res.end();
      return Readable.fromWeb(upstream.body).pipe(res);
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/asr') {
      const device = requireDevice(req, res);
      if (!device) return;
      const selected = activeProvider('asr');
      if (!selected) return json(res, 503, { error: 'asr_not_configured' });
      if (selected.provider !== 'qwen') return json(res, 501, { error: 'asr_provider_not_implemented' });
      const format = String(url.searchParams.get('format') || 'pcm').toLowerCase();
      const sampleRate = Number(url.searchParams.get('sampleRate') || 16000);
      if (!['pcm', 'wav', 'mp3', 'opus', 'speex', 'aac', 'amr'].includes(format)) return json(res, 400, { error: 'unsupported_audio_format' });
      if (!Number.isInteger(sampleRate) || sampleRate < 8000 || sampleRate > 48000) return json(res, 400, { error: 'invalid_sample_rate' });
      const audio = await readBody(req, config.speechMaxBytes || 4 * 1024 * 1024);
      if (!audio.length) return json(res, 400, { error: 'audio_required' });
      try {
        const result = await recognizeWithQwen({ selected, credentials: providerCredentials(selected), audio, format, sampleRate,
          webSocketFactory: config.webSocketFactory, frameDelayMs: config.asrFrameDelayMs ?? 100 });
        return json(res, 200, result);
      } catch (error) {
        return json(res, error.status || 502, { error: error.code || error.message || 'asr_upstream_failed' });
      }
    }

    if (req.method === 'POST' && url.pathname === '/api/device/v1/tts') {
      const device = requireDevice(req, res);
      if (!device) return;
      const selected = activeProvider('tts');
      if (!selected) return json(res, 503, { error: 'tts_not_configured' });
      if (selected.provider !== 'volcengine') return json(res, 501, { error: 'tts_provider_not_implemented' });
      const body = await readJson(req, 32 * 1024);
      const text = typeof body.text === 'string' ? body.text.trim() : '';
      if (!text || text.length > (config.ttsMaxCharacters || 2000)) return json(res, 400, { error: 'tts_text_invalid' });
      try {
        const result = await synthesizeWithVolcengine({ selected, credentials: providerCredentials(selected), text, deviceId: device.device_id });
        const iterator = result.audio[Symbol.asyncIterator]();
        const first = await iterator.next();
        if (first.done) return json(res, 502, { error: 'tts_upstream_empty_response' });
        res.writeHead(200, { 'content-type': result.contentType, 'cache-control': 'no-store', 'x-audio-format': result.format,
          'x-audio-sample-rate': String(result.sampleRate), 'x-content-type-options': 'nosniff' });
        res.write(first.value);
        for await (const chunk of { [Symbol.asyncIterator]: () => iterator }) res.write(chunk);
        return res.end();
      } catch (error) {
        if (res.headersSent) return res.destroy(error);
        return json(res, error.status || 502, { error: error.code || error.message || 'tts_upstream_failed' });
      }
    }

    return false;
  }

  function providerRevision(database) {
    return database.prepare("SELECT COALESCE(MAX(updated_at),'') AS revision FROM provider_configs").get().revision;
  }

  function serveStatic(req, res, url) {
    const route = url.pathname === '/' || url.pathname === '/account' || url.pathname === '/account/' ? '/index.html' : url.pathname;
    const path = resolve(webRoot, `.${route}`);
    if (!path.startsWith(`${webRoot}/`)) return json(res, 404, { error: 'not_found' });
    try {
      const stat = statSync(path);
      if (!stat.isFile()) throw new Error();
      const types = { '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.svg': 'image/svg+xml' };
      const extension = extname(path);
      res.writeHead(200, { 'content-type': types[extension] || 'application/octet-stream', 'content-length': stat.size,
        'cache-control': ['.html', '.css', '.js'].includes(extension) ? 'no-store' : 'public, max-age=3600' });
      if (req.method === 'HEAD') return res.end();
      createReadStream(path).pipe(res);
    } catch {
      json(res, 404, { error: 'not_found' });
    }
  }

  const server = createServer(async (req, res) => {
    securityHeaders(res);
    const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
    try {
      if (url.pathname === '/healthz' || url.pathname.startsWith('/api/')) {
        const handled = await handleApi(req, res, url);
        if (handled === false) json(res, 404, { error: 'not_found' });
      } else if (req.method === 'GET' || req.method === 'HEAD') {
        serveStatic(req, res, url);
      } else {
        json(res, 405, { error: 'method_not_allowed' });
      }
    } catch (error) {
      console.error(error);
      if (!res.headersSent) json(res, error.status || 500, { error: error.status ? error.message : 'internal_server_error' });
      else res.destroy();
    }
  });

  attachVoiceSocket(server, { authenticate: authenticateDeviceAuthorization,
    speechMaxBytes: config.speechMaxBytes || 4 * 1024 * 1024 });
  server.on('close', () => db.close());
  return { server, db };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  try {
    const config = loadConfig();
    const { server } = createApp(config);
    server.listen(config.port, config.host, () => console.log(`baby-tank-cloud listening on http://${config.host}:${config.port}`));
  } catch (error) {
    console.error(`startup failed: ${error.message}`);
    process.exitCode = 1;
  }
}
