import { createCipheriv, createDecipheriv, createHash, randomBytes, scryptSync, timingSafeEqual } from 'node:crypto';

export function json(res, status, body, extraHeaders = {}) {
  const payload = Buffer.from(JSON.stringify(body));
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': payload.length,
    ...extraHeaders,
  });
  res.end(payload);
}

export async function readBody(req, limit = 1024 * 1024) {
  const chunks = [];
  let size = 0;
  for await (const chunk of req) {
    size += chunk.length;
    if (size > limit) {
      const error = new Error('request body too large');
      error.status = 413;
      throw error;
    }
    chunks.push(chunk);
  }
  return Buffer.concat(chunks);
}

export async function readJson(req, limit) {
  const body = await readBody(req, limit);
  if (!body.length) return {};
  try {
    return JSON.parse(body.toString('utf8'));
  } catch {
    const error = new Error('invalid JSON');
    error.status = 400;
    throw error;
  }
}

export function hashToken(token) {
  return createHash('sha256').update(token).digest('hex');
}

export function newToken(bytes = 32) {
  return randomBytes(bytes).toString('base64url');
}

export function hashPassword(password, salt = randomBytes(16)) {
  return `scrypt$${salt.toString('base64')}$${scryptSync(password, salt, 64).toString('base64')}`;
}

export function verifyPassword(password, stored) {
  const [algorithm, salt64, hash64] = String(stored).split('$');
  if (algorithm !== 'scrypt' || !salt64 || !hash64) return false;
  const expected = Buffer.from(hash64, 'base64');
  const actual = scryptSync(password, Buffer.from(salt64, 'base64'), expected.length);
  return expected.length === actual.length && timingSafeEqual(expected, actual);
}

export function createSecretBox(key) {
  return {
    encrypt(plainText) {
      if (!plainText) return null;
      const iv = randomBytes(12);
      const cipher = createCipheriv('aes-256-gcm', key, iv);
      const encrypted = Buffer.concat([cipher.update(String(plainText), 'utf8'), cipher.final()]);
      return Buffer.concat([iv, cipher.getAuthTag(), encrypted]).toString('base64');
    },
    decrypt(value) {
      if (!value) return '';
      const packed = Buffer.from(value, 'base64');
      const iv = packed.subarray(0, 12);
      const tag = packed.subarray(12, 28);
      const encrypted = packed.subarray(28);
      const decipher = createDecipheriv('aes-256-gcm', key, iv);
      decipher.setAuthTag(tag);
      return Buffer.concat([decipher.update(encrypted), decipher.final()]).toString('utf8');
    },
  };
}

export function parseCookies(header = '') {
  return Object.fromEntries(header.split(';').map((part) => part.trim()).filter(Boolean).map((part) => {
    const index = part.indexOf('=');
    return [decodeURIComponent(part.slice(0, index)), decodeURIComponent(part.slice(index + 1))];
  }));
}

export function isSafeName(value, max = 80) {
  return typeof value === 'string' && value.length > 0 && value.length <= max && /^[a-zA-Z0-9._-]+$/.test(value);
}

export function compareVersions(a, b) {
  const parse = (value) => String(value).replace(/^v/, '').split(/[.+-]/).map((part) => /^\d+$/.test(part) ? Number(part) : part);
  const left = parse(a);
  const right = parse(b);
  for (let i = 0; i < Math.max(left.length, right.length); i += 1) {
    const x = left[i] ?? 0;
    const y = right[i] ?? 0;
    if (x === y) continue;
    if (typeof x === 'number' && typeof y === 'number') return x > y ? 1 : -1;
    return String(x).localeCompare(String(y), 'en', { numeric: true });
  }
  return 0;
}

export function now() {
  return new Date().toISOString();
}
