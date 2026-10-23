import { randomUUID } from 'node:crypto';
import { DEFAULT_NEEDS, EVENT_RULES, applyNeedEvent, deriveCharacterState,
  projectNeeds, validTimeZone } from './state_engine.mjs';

function rowFor(db, deviceId) {
  return db.prepare('SELECT * FROM character_state WHERE device_id=?').get(deviceId);
}

function initialize(db, deviceId, at) {
  db.prepare(`INSERT OR IGNORE INTO character_state(device_id,needs_json,updated_at)
    VALUES (?,?,?)`).run(deviceId, JSON.stringify(DEFAULT_NEEDS), at);
  return rowFor(db, deviceId);
}

function snapshot(row, at) {
  const policy = { timeZone: row.time_zone, deepNightLock: row.deep_night_lock === 1 };
  const needs = projectNeeds(JSON.parse(row.needs_json), row.updated_at, at, policy.timeZone);
  return { revision: row.revision, policyRevision: row.policy_revision,
    updatedAt: row.updated_at,
    enforcement: 'preview_only',
    policy, ...deriveCharacterState(needs, at, policy) };
}

export function readCharacterState(db, deviceId, at = new Date().toISOString()) {
  return snapshot(initialize(db, deviceId, at), at);
}

export function recordCharacterEvent(db, deviceId, { eventId, kind, source },
                                     at = new Date().toISOString()) {
  if (typeof eventId !== 'string' || !/^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$/.test(eventId) ||
      !EVENT_RULES[kind] || !['device', 'parent', 'conversation'].includes(source)) {
    throw new RangeError('invalid_character_event');
  }
  const allowed = {
    device: ['side_touch', 'scene_completed'],
    parent: ['fed', 'drank', 'played', 'poked', 'rested'],
    conversation: ['praised', 'shared_discovery', 'comforted'],
  };
  if (!allowed[source].includes(kind)) throw new RangeError('invalid_character_event');
  db.exec('BEGIN IMMEDIATE');
  try {
    const row = initialize(db, deviceId, at);
    const duplicate = db.prepare(`SELECT id FROM character_state_events
      WHERE device_id=? AND event_id=?`).get(deviceId, eventId);
    if (duplicate) {
      const state = snapshot(row, at);
      db.exec('COMMIT');
      return { duplicate: true, state };
    }
    if (snapshot(row, at).deepNightGateRequested &&
        source !== 'conversation') {
      throw new RangeError('character_night_locked');
    }
    const hourAgo = new Date(new Date(at).getTime() - 3_600_000).toISOString();
    const recent = db.prepare(`SELECT COUNT(*) AS count FROM character_state_events
      WHERE device_id=? AND source=? AND kind=? AND created_at>?`)
      .get(deviceId, source, kind, hourAgo).count;
    if (recent >= (kind === 'side_touch' || kind === 'poked' ? 20 : 12)) {
      throw new RangeError('character_event_rate_limited');
    }
    if (kind === 'scene_completed') {
      const dayAgo = new Date(new Date(at).getTime() - 86_400_000).toISOString();
      const daily = db.prepare(`SELECT COUNT(*) AS count FROM character_state_events
        WHERE device_id=? AND kind='scene_completed' AND created_at>?`)
        .get(deviceId, dayAgo).count;
      if (daily >= 8) throw new RangeError('character_event_rate_limited');
    }
    const current = projectNeeds(JSON.parse(row.needs_json), row.updated_at, at, row.time_zone);
    const next = applyNeedEvent(current, kind);
    const revision = row.revision + 1;
    db.prepare(`UPDATE character_state SET needs_json=?,revision=?,updated_at=?
      WHERE device_id=?`).run(JSON.stringify(next), revision, at, deviceId);
    db.prepare(`INSERT INTO character_state_events(device_id,event_id,source,kind,revision,created_at)
      VALUES (?,?,?,?,?,?)`).run(deviceId, eventId, source, kind, revision, at);
    // Retain 30 days of event metadata. No child utterance is stored here.
    db.prepare(`DELETE FROM character_state_events WHERE device_id=? AND created_at<?`)
      .run(deviceId, new Date(new Date(at).getTime() - 30 * 86_400_000).toISOString());
    const state = snapshot(rowFor(db, deviceId), at);
    db.exec('COMMIT');
    return { duplicate: false, state };
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
}

export function setCharacterPolicy(db, deviceId, { timeZone, deepNightLock, revision },
                                   at = new Date().toISOString()) {
  if (!validTimeZone(timeZone) || typeof deepNightLock !== 'boolean' ||
      !Number.isInteger(revision) || revision < 1) {
    throw new RangeError('invalid_character_policy');
  }
  db.exec('BEGIN IMMEDIATE');
  try {
    const row = initialize(db, deviceId, at);
    if (row.policy_revision !== revision) throw new RangeError('character_policy_conflict');
    const next = row.revision + 1;
    const needs = projectNeeds(JSON.parse(row.needs_json), row.updated_at, at, row.time_zone);
    db.prepare(`UPDATE character_state SET needs_json=?,time_zone=?,deep_night_lock=?,
      revision=?,policy_revision=?,updated_at=? WHERE device_id=?`).run(JSON.stringify(needs),
      timeZone, deepNightLock ? 1 : 0, next, row.policy_revision + 1,
      at, deviceId);
    const state = snapshot(rowFor(db, deviceId), at);
    db.exec('COMMIT');
    return state;
  } catch (error) {
    if (db.isTransaction) db.exec('ROLLBACK');
    throw error;
  }
}

export function newCharacterEventId() { return randomUUID(); }
