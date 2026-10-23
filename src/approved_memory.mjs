import { randomUUID } from 'node:crypto';

const subjects = Object.freeze({
  nickname: new Set(['name']),
  preference: new Set(['story_theme', 'play_theme']),
  shared_activity: new Set(['activity']),
});

function validate(kind, subject, value) {
  const clean = typeof value === 'string' ? value.trim() : '';
  if (!subjects[kind]?.has(subject) || !clean || [...clean].length > 40 ||
      /[\u0000-\u001f\u007f<>]/u.test(clean)) {
    throw new RangeError('invalid_memory');
  }
  return clean;
}

function decode(secrets, row) {
  try {
    const value = secrets.decrypt(row.value_ciphertext);
    if (!value) return null;
    return { id: row.id, kind: row.kind, subject: row.subject, value,
      createdAt: row.created_at, updatedAt: row.updated_at,
      expiresAt: row.expires_at, revision: row.revision };
  } catch { return null; }
}

export function listApprovedMemories(db, secrets, deviceId,
                                     at = new Date().toISOString()) {
  return db.prepare(`SELECT * FROM approved_memories WHERE device_id=?
    AND (expires_at IS NULL OR expires_at>?)
    ORDER BY updated_at DESC LIMIT 50`).all(deviceId, at)
    .map((row) => decode(secrets, row)).filter(Boolean);
}

export function addApprovedMemory(db, secrets, deviceId, { kind, subject, value },
                                  at = new Date().toISOString()) {
  const clean = validate(kind, subject, value);
  const expiresAt = kind === 'shared_activity' ?
    new Date(new Date(at).getTime() + 30 * 86_400_000).toISOString() : null;
  const id = randomUUID();
  db.exec('BEGIN IMMEDIATE');
  try {
    // Replace the encrypted old value, not merely hide it from retrieval.
    db.prepare(`DELETE FROM approved_memories WHERE device_id=?
      AND kind=? AND subject=?`).run(deviceId, kind, subject);
    db.prepare(`INSERT INTO approved_memories(id,device_id,kind,subject,
      value_ciphertext,created_at,updated_at,expires_at) VALUES (?,?,?,?,?,?,?,?)`)
      .run(id, deviceId, kind, subject, secrets.encrypt(clean), at, at, expiresAt);
    db.exec('COMMIT');
    return { id, kind, subject, value: clean, createdAt: at,
      updatedAt: at, expiresAt, revision: 1 };
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
}

export function revokeApprovedMemory(db, deviceId, id,
                                     at = new Date().toISOString()) {
  void at;
  return db.prepare(`DELETE FROM approved_memories WHERE id=? AND device_id=?`)
    .run(id, deviceId).changes === 1;
}

export function updateApprovedMemory(db, secrets, deviceId, id,
                                     { value, revision },
                                     at = new Date().toISOString()) {
  if (!Number.isInteger(revision) || revision < 1) {
    throw new RangeError('invalid_memory_revision');
  }
  db.exec('BEGIN IMMEDIATE');
  try {
    const row = db.prepare(`SELECT * FROM approved_memories
      WHERE id=? AND device_id=?`).get(id, deviceId);
    if (!row) throw new RangeError('memory_not_found');
    if (row.revision !== revision) throw new RangeError('memory_conflict');
    const clean = validate(row.kind, row.subject, value);
    db.prepare(`UPDATE approved_memories SET value_ciphertext=?,revision=?,
      updated_at=? WHERE id=? AND device_id=?`).run(secrets.encrypt(clean),
      revision + 1, at, id, deviceId);
    db.exec('COMMIT');
    return decode(secrets, db.prepare(`SELECT * FROM approved_memories
      WHERE id=? AND device_id=?`).get(id, deviceId));
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
}

export function clearApprovedMemories(db, deviceId) {
  return db.prepare('DELETE FROM approved_memories WHERE device_id=?')
    .run(deviceId).changes;
}

export function relevantApprovedMemories(db, secrets, deviceId, userText,
                                          at = new Date().toISOString()) {
  const text = String(userText || '');
  const relevant = listApprovedMemories(db, secrets, deviceId, at).filter((item) => {
    if (item.kind === 'nickname') return true;
    if (item.subject === 'story_theme') return /故事|讲讲|读书|绘本/u.test(text);
    if (item.subject === 'play_theme') return /玩|游戏|活动/u.test(text);
    return /之前|上次|记得|做过/u.test(text);
  });
  return relevant.slice(0, 3).map(({ kind, subject, value }) => ({ kind, subject, value }));
}

export function pruneExpiredMemories(db, at = new Date().toISOString()) {
  return db.prepare(`DELETE FROM approved_memories WHERE expires_at IS NOT NULL
    AND expires_at<=?`).run(at).changes;
}
