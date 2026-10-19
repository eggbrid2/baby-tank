import { resolve } from 'node:path';

export function loadConfig(env = process.env) {
  const rawKey = env.MASTER_KEY ? Buffer.from(env.MASTER_KEY, 'base64') : null;
  if (!rawKey || rawKey.length !== 32) throw new Error('MASTER_KEY must be exactly 32 random bytes encoded as base64');
  const dataDir = resolve(env.DATA_DIR || './data');
  return {
    host: env.HOST || '127.0.0.1',
    port: Number(env.PORT || 8787),
    dataDir,
    dbPath: resolve(dataDir, 'baby-tank.sqlite3'),
    firmwareDir: resolve(dataDir, 'firmware'),
    adminUsername: env.ADMIN_USERNAME || 'admin',
    adminPassword: env.ADMIN_PASSWORD || '',
    masterKey: rawKey,
    cookieSecure: env.COOKIE_SECURE !== 'false',
    publicBaseUrl: (env.PUBLIC_BASE_URL || '').replace(/\/$/, ''),
    firmwareMaxBytes: Number(env.FIRMWARE_MAX_BYTES || 32 * 1024 * 1024),
    speechMaxBytes: Number(env.SPEECH_MAX_BYTES || 4 * 1024 * 1024),
    ttsMaxCharacters: Number(env.TTS_MAX_CHARACTERS || 2000),
  };
}
