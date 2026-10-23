import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openDatabase } from '../src/db.mjs';
import { readCharacterState, recordCharacterEvent, setCharacterPolicy } from '../src/character_state.mjs';
import { DEFAULT_NEEDS, applyNeedEvent, deriveCharacterState,
  inferConversationNeedEvent,
  nightLockWindows, projectNeeds, timeBand } from '../src/state_engine.mjs';

test('local schedule boundaries and the optional deep-night gate', () => {
  const zone = 'Asia/Shanghai';
  assert.equal(timeBand('2026-10-20T12:59:00Z', zone), 'early_evening');
  assert.equal(timeBand('2026-10-20T13:00:00Z', zone), 'evening');
  assert.equal(timeBand('2026-10-20T15:00:00Z', zone), 'late_night');
  assert.equal(timeBand('2026-10-20T21:59:00Z', zone), 'late_night');
  assert.equal(timeBand('2026-10-20T22:00:00Z', zone), 'morning');
  assert.equal(deriveCharacterState(DEFAULT_NEEDS, '2026-10-20T15:00:00Z',
    { timeZone: zone, deepNightLock: true }).deepNightGateRequested, true);
  assert.equal(deriveCharacterState(DEFAULT_NEEDS, '2026-10-20T15:00:00Z',
    { timeZone: zone, deepNightLock: false }).deepNightGateRequested, false);
});

test('head receives UTC lock windows across local boundaries and DST', () => {
  const shanghai = nightLockWindows('2026-10-20T14:50:00.000Z',
    'Asia/Shanghai', 1);
  assert.deepEqual(shanghai.windowsUtc[0], [
    '2026-10-20T15:00:00.000Z', '2026-10-20T22:00:00.000Z']);
  const newYork = nightLockWindows('2026-11-01T00:00:00.000Z',
    'America/New_York', 2);
  assert.ok(newYork.windowsUtc.some(([from, to]) =>
    from === '2026-11-01T03:00:00.000Z' &&
    to === '2026-11-01T11:00:00.000Z'));
});

test('need projection is bounded, deterministic, and model emotion cannot set it', () => {
  const first = projectNeeds(DEFAULT_NEEDS, '2026-10-19T00:00:00Z',
    '2026-10-20T00:00:00Z');
  const stale = projectNeeds(DEFAULT_NEEDS, '2026-10-01T00:00:00Z',
    '2026-10-20T00:00:00Z');
  assert.deepEqual(stale, first, 'offline catch-up stops at 24 hours');
  assert.ok(first.hunger > DEFAULT_NEEDS.hunger);
  assert.ok(first.hunger <= 68);
  assert.ok(first.thirst <= 68);
  assert.equal(applyNeedEvent(DEFAULT_NEEDS, 'fed').hunger, 5);
  assert.equal(applyNeedEvent(DEFAULT_NEEDS, 'poked').bond, 57);
  assert.equal(applyNeedEvent({ ...DEFAULT_NEEDS, energy: 30 }, 'rested').energy, 54);
  assert.throws(() => applyNeedEvent(DEFAULT_NEEDS, 'angry'), /invalid_state_event/);
  assert.deepEqual(projectNeeds(DEFAULT_NEEDS, '2026-10-20T00:00:00Z',
    '2026-10-19T00:00:00Z'), DEFAULT_NEEDS, 'backward clock must not decay');
});

test('needs take priority and positive moods require stronger evidence', () => {
  const at = '2026-10-20T04:00:00Z';
  const policy = { timeZone: 'Asia/Shanghai' };
  assert.equal(deriveCharacterState({ ...DEFAULT_NEEDS, energy: 25, stamina: 25 },
    at, policy).mood, 'sleepy');
  assert.equal(deriveCharacterState({ ...DEFAULT_NEEDS, hunger: 64,
    excitement: 60 }, at, policy).mood, 'annoyed');
  assert.equal(deriveCharacterState({ ...DEFAULT_NEEDS, hunger: 62, thirst: 62,
    energy: 40 }, at, policy).mood, 'worried');
  assert.equal(deriveCharacterState({ ...DEFAULT_NEEDS, curiosity: 78,
    excitement: 42 }, at, policy).mood, 'curious');
  assert.equal(deriveCharacterState({ ...DEFAULT_NEEDS, joy: 78,
    bond: 75, excitement: 48 }, at, policy).mood, 'happy');
  assert.equal(deriveCharacterState({ ...DEFAULT_NEEDS, joy: 78,
    excitement: 84, energy: 76 }, at, policy).mood, 'excited');
});

test('conversation changes needs only for narrow completed-turn evidence', () => {
  assert.equal(inferConversationNeedEvent('谢谢你', '不用谢呀。'), 'praised');
  assert.equal(inferConversationNeedEvent('为什么星星会闪？', '因为大气会扰动星光。'),
    'shared_discovery');
  assert.equal(inferConversationNeedEvent('我饿了', '要不要告诉大人？'), null);
  assert.equal(inferConversationNeedEvent('为什么', '错误'), null);
  assert.equal(inferConversationNeedEvent('谢谢你', ''), null);
});

test('state events are idempotent and policy writes use revision checks', (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'baby-tank-state-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const db = openDatabase(join(dir, 'state.sqlite3'),
    { username: 'admin', password: 'test-password-long-enough' });
  t.after(() => db.close());
  const created = '2026-10-20T04:00:00.000Z';
  const device = db.prepare(`INSERT INTO devices(device_id,name,board,token_hash,created_at)
    VALUES (?,?,?,?,?)`).run('tank-test', 'Test', 's3', 'hash', created);
  const id = Number(device.lastInsertRowid);
  const initial = readCharacterState(db, id, created);
  assert.equal(initial.revision, 1);
  assert.equal(initial.policyRevision, 1);
  const input = { eventId: '40af57e5-7e51-4c30-8792-81747864ce00',
    kind: 'fed', source: 'parent' };
  const first = recordCharacterEvent(db, id, input, created);
  assert.equal(first.duplicate, false);
  assert.equal(first.state.needs.hunger, 5);
  assert.equal(first.state.policyRevision, 1);
  const retry = recordCharacterEvent(db, id, input, created);
  assert.equal(retry.duplicate, true);
  assert.equal(retry.state.needs.hunger, 5);
  assert.equal(retry.state.revision, 2);
  assert.throws(() => setCharacterPolicy(db, id, {
    timeZone: 'Asia/Shanghai', deepNightLock: true, revision: 2,
  }, created), /character_policy_conflict/);
  const changed = setCharacterPolicy(db, id, {
    timeZone: 'Asia/Shanghai', deepNightLock: true, revision: 1,
  }, created);
  assert.equal(changed.revision, 3);
  assert.equal(changed.policyRevision, 2);
  assert.equal(changed.policy.deepNightLock, true);
  assert.equal(db.prepare('SELECT COUNT(*) AS n FROM character_state_events').get().n, 1);
  const appPoke = recordCharacterEvent(db, id, {
    eventId: '1bc15451-1115-4eb0-ad6d-1d9e6228371b',
    kind: 'poked', source: 'parent',
  }, created);
  assert.equal(appPoke.state.needs.bond, 57);
});
