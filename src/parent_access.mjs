import { randomBytes } from 'node:crypto';
import { hashToken, newToken } from './lib.mjs';

const CLAIM_MS = 10 * 60_000;
const SESSION_MS = 30 * 86_400_000;

export function createParentClaim(db, deviceId, at = new Date()) {
  const code = randomBytes(8).toString('hex').toUpperCase();
  const readable = code.match(/.{1,4}/g).join('-');
  const createdAt = at.toISOString();
  const expiresAt = new Date(at.getTime() + CLAIM_MS).toISOString();
  db.exec('BEGIN IMMEDIATE');
  try {
    db.prepare('DELETE FROM parent_claims WHERE device_id=?').run(deviceId);
    db.prepare(`INSERT INTO parent_claims(code_hash,device_id,expires_at,created_at)
      VALUES (?,?,?,?)`).run(hashToken(code), deviceId, expiresAt, createdAt);
    db.exec('COMMIT');
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
  return { code: readable, expiresAt };
}

export function claimParentAccess(db, value, at = new Date()) {
  const code = typeof value === 'string' ? value.replace(/[\s-]/gu, '').toUpperCase() : '';
  if (!/^[0-9A-F]{16}$/.test(code)) return null;
  db.exec('BEGIN IMMEDIATE');
  try {
    const row = db.prepare(`SELECT c.device_id,d.device_id AS public_id
      FROM parent_claims c JOIN devices d ON d.id=c.device_id
      WHERE c.code_hash=? AND c.consumed_at IS NULL AND c.expires_at>?
      AND d.revoked_at IS NULL`).get(hashToken(code), at.toISOString());
    if (!row) {
      db.exec('COMMIT');
      return null;
    }
    const token = newToken();
    const expiresAt = new Date(at.getTime() + SESSION_MS).toISOString();
    db.prepare(`UPDATE parent_claims SET consumed_at=? WHERE code_hash=?`)
      .run(at.toISOString(), hashToken(code));
    db.prepare(`INSERT INTO parent_sessions(token_hash,device_id,expires_at,created_at)
      VALUES (?,?,?,?)`).run(hashToken(token), row.device_id,
        expiresAt, at.toISOString());
    db.exec('COMMIT');
    return { token, deviceId: row.public_id, expiresAt };
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
}

export function authenticateParent(db, authorization, at = new Date()) {
  const match = /^Bearer\s+([A-Za-z0-9_-]{32,})$/u.exec(String(authorization || ''));
  if (!match) return null;
  return db.prepare(`SELECT s.token_hash,s.device_id,d.device_id AS public_id
    FROM parent_sessions s JOIN devices d ON d.id=s.device_id
    WHERE s.token_hash=? AND s.expires_at>? AND s.revoked_at IS NULL
    AND d.revoked_at IS NULL`).get(hashToken(match[1]), at.toISOString()) || null;
}

export function revokeParentAccess(db, authorization, at = new Date()) {
  const session = authenticateParent(db, authorization, at);
  if (!session) return false;
  return db.prepare(`UPDATE parent_sessions SET revoked_at=? WHERE token_hash=?`)
    .run(at.toISOString(), session.token_hash).changes === 1;
}

export function revokeDeviceParents(db, deviceId, at = new Date()) {
  const changed = db.prepare(`UPDATE parent_sessions SET revoked_at=?
    WHERE device_id=? AND revoked_at IS NULL`).run(at.toISOString(), deviceId).changes;
  db.prepare('DELETE FROM parent_claims WHERE device_id=?').run(deviceId);
  return changed;
}
