export function readQuestionHistoryPolicy(db, deviceId) {
  const row = db.prepare(`SELECT enabled,retention_days,updated_at
    FROM question_history_policy WHERE device_id=?`).get(deviceId);
  return { enabled: row?.enabled === 1, retentionDays: row?.retention_days || 30,
    updatedAt: row?.updated_at || null };
}

export function setQuestionHistoryPolicy(db, deviceId, { enabled, retentionDays },
                                         at = new Date().toISOString()) {
  if (typeof enabled !== 'boolean' || !Number.isInteger(retentionDays) ||
      retentionDays < 1 || retentionDays > 30) {
    throw new RangeError('invalid_question_history_policy');
  }
  db.exec('BEGIN IMMEDIATE');
  try {
    db.prepare(`INSERT INTO question_history_policy(device_id,enabled,retention_days,updated_at)
      VALUES (?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET
      enabled=excluded.enabled,retention_days=excluded.retention_days,
      updated_at=excluded.updated_at`).run(deviceId, enabled ? 1 : 0,
      retentionDays, at);
    if (!enabled) {
      db.prepare('DELETE FROM question_history WHERE device_id=?').run(deviceId);
    } else {
      const cutoff = new Date(new Date(at).getTime() - retentionDays * 86_400_000)
        .toISOString();
      db.prepare('DELETE FROM question_history WHERE device_id=? AND created_at<?')
        .run(deviceId, cutoff);
    }
    db.exec('COMMIT');
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
  return readQuestionHistoryPolicy(db, deviceId);
}

export function archiveQuestion(db, secrets, deviceId, traceId, question, answer,
                                at = new Date().toISOString()) {
  const policy = readQuestionHistoryPolicy(db, deviceId);
  if (!policy.enabled || !question?.trim() || !answer?.trim()) return false;
  const cutoff = new Date(new Date(at).getTime() - policy.retentionDays * 86_400_000)
    .toISOString();
  db.exec('BEGIN IMMEDIATE');
  try {
    // Re-read under the write lock so an opt-out cannot race with an archive.
    if (readQuestionHistoryPolicy(db, deviceId).enabled) {
      db.prepare(`INSERT OR IGNORE INTO question_history(device_id,trace_id,
        question_ciphertext,answer_ciphertext,created_at) VALUES (?,?,?,?,?)`)
        .run(deviceId, traceId, secrets.encrypt(String(question).slice(0, 384)),
          secrets.encrypt(String(answer).slice(0, 1024)), at);
    }
    db.prepare('DELETE FROM question_history WHERE device_id=? AND created_at<?')
      .run(deviceId, cutoff);
    db.exec('COMMIT');
    return true;
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
}

export function listQuestions(db, secrets, deviceId, { beforeId = null, limit = 20 } = {},
                              at = new Date().toISOString()) {
  if (!Number.isInteger(limit) || limit < 1 || limit > 50 ||
      (beforeId !== null && (!Number.isInteger(beforeId) || beforeId < 1))) {
    throw new RangeError('invalid_question_history_page');
  }
  const policy = readQuestionHistoryPolicy(db, deviceId);
  const cutoff = new Date(new Date(at).getTime() - policy.retentionDays * 86_400_000)
    .toISOString();
  const rows = db.prepare(`SELECT id,trace_id,question_ciphertext,
    answer_ciphertext,created_at FROM question_history WHERE device_id=?
    AND created_at>=? AND (? IS NULL OR id<?) ORDER BY id DESC LIMIT ?`)
    .all(deviceId, cutoff, beforeId, beforeId, limit);
  return rows.flatMap((row) => {
    try {
      return [{ id: row.id, traceId: row.trace_id,
        question: secrets.decrypt(row.question_ciphertext),
        answer: secrets.decrypt(row.answer_ciphertext), createdAt: row.created_at }];
    } catch { return []; }
  });
}

export function deleteQuestion(db, deviceId, id = null) {
  if (id !== null && (!Number.isInteger(id) || id < 1)) {
    throw new RangeError('invalid_question_id');
  }
  return id === null ?
    db.prepare('DELETE FROM question_history WHERE device_id=?').run(deviceId).changes :
    db.prepare('DELETE FROM question_history WHERE device_id=? AND id=?')
      .run(deviceId, id).changes;
}

export function pruneExpiredQuestions(db, at = new Date().toISOString()) {
  let removed = 0;
  for (const row of db.prepare(`SELECT device_id,enabled,retention_days
    FROM question_history_policy`).all()) {
    if (row.enabled !== 1) {
      removed += db.prepare('DELETE FROM question_history WHERE device_id=?')
        .run(row.device_id).changes;
      continue;
    }
    const cutoff = new Date(new Date(at).getTime() - row.retention_days * 86_400_000)
      .toISOString();
    removed += db.prepare(`DELETE FROM question_history WHERE device_id=?
      AND created_at<?`).run(row.device_id, cutoff).changes;
  }
  return removed;
}
