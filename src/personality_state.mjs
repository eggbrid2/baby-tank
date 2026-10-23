export const DEFAULT_PERSONALITY = Object.freeze({
  curiosity: 78, warmth: 84, talkativeness: 46,
});

function rowFor(db, deviceId) {
  return db.prepare('SELECT * FROM character_personality WHERE device_id=?').get(deviceId);
}

export function readPersonality(db, deviceId, at = new Date().toISOString()) {
  db.prepare(`INSERT OR IGNORE INTO character_personality
    (device_id,curiosity,warmth,talkativeness,updated_at)
    VALUES (?,?,?,?,?)`).run(deviceId, DEFAULT_PERSONALITY.curiosity,
    DEFAULT_PERSONALITY.warmth, DEFAULT_PERSONALITY.talkativeness, at);
  const row = rowFor(db, deviceId);
  return { curiosity: row.curiosity, warmth: row.warmth,
    talkativeness: row.talkativeness, revision: row.revision,
    updatedAt: row.updated_at };
}

export function setPersonality(db, deviceId, input,
                               at = new Date().toISOString()) {
  const traits = ['curiosity', 'warmth', 'talkativeness'];
  if (!Number.isInteger(input?.revision) || input.revision < 1 ||
      traits.some((key) => !Number.isInteger(input?.[key]) ||
        input[key] < 0 || input[key] > 100)) {
    throw new RangeError('invalid_personality');
  }
  db.exec('BEGIN IMMEDIATE');
  try {
    const current = readPersonality(db, deviceId, at);
    if (input.revision !== current.revision) {
      throw new RangeError('personality_conflict');
    }
    db.prepare(`UPDATE character_personality SET curiosity=?,warmth=?,
      talkativeness=?,revision=?,source='parent',updated_at=? WHERE device_id=?`)
      .run(input.curiosity, input.warmth, input.talkativeness,
        current.revision + 1, at, deviceId);
    db.exec('COMMIT');
    return readPersonality(db, deviceId, at);
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
}

export function bootstrapPersonalityFromDevice(db, deviceId, input,
                                               at = new Date().toISOString()) {
  const traits = ['curiosity', 'warmth', 'talkativeness'];
  if (traits.some((key) => !Number.isInteger(input?.[key]) ||
      input[key] < 0 || input[key] > 100)) {
    throw new RangeError('invalid_personality');
  }
  db.exec('BEGIN IMMEDIATE');
  try {
    readPersonality(db, deviceId, at);
    db.prepare(`UPDATE character_personality SET curiosity=?,warmth=?,
      talkativeness=?,revision=revision+1,source='device',updated_at=?
      WHERE device_id=? AND source='default'`).run(input.curiosity,
      input.warmth, input.talkativeness, at, deviceId);
    db.exec('COMMIT');
    return readPersonality(db, deviceId, at);
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
}
