import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openDatabase } from '../src/db.mjs';
import { readDeviceStateAck, recordDeviceStateAck } from '../src/device_state_ack.mjs';
import { bootstrapPersonalityFromDevice, readPersonality,
  setPersonality } from '../src/personality_state.mjs';

test('personality uses an authoritative revision and device confirms applied versions', () => {
  const dir = mkdtempSync(join(tmpdir(), 'baby-persona-'));
  const db = openDatabase(join(dir, 'state.sqlite'),
    { password: 'test-password-with-adequate-length' });
  try {
    db.prepare(`INSERT INTO devices(device_id,name,board,token_hash,created_at)
      VALUES ('head-1','Head','esp32s3','hash','2026-10-20T00:00:00.000Z')`).run();
    const deviceId = db.prepare("SELECT id FROM devices WHERE device_id='head-1'").get().id;
    const initial = readPersonality(db, deviceId);
    assert.equal(initial.revision, 1);
    const imported = bootstrapPersonalityFromDevice(db, deviceId,
      { curiosity: 81, warmth: 77, talkativeness: 49 });
    assert.equal(imported.revision, 2);
    assert.equal(imported.curiosity, 81);
    const next = setPersonality(db, deviceId, { curiosity: 90, warmth: 70,
      talkativeness: 50, revision: imported.revision });
    assert.equal(next.revision, 3);
    assert.equal(bootstrapPersonalityFromDevice(db, deviceId,
      { curiosity: 1, warmth: 1, talkativeness: 1 }).curiosity, 90);
    assert.throws(() => setPersonality(db, deviceId, { curiosity: 1, warmth: 1,
      talkativeness: 1, revision: 1 }), /personality_conflict/);
    assert.equal(readPersonality(db, deviceId).curiosity, 90);
    const ack = recordDeviceStateAck(db, deviceId, { policyRevision: 1,
      personalityRevision: 3, clockQuality: 'verified',
      nightLockActive: false }, '2026-10-20T00:00:00.000Z');
    assert.equal(ack.personalityRevision, 3);
    assert.equal(readDeviceStateAck(db, deviceId,
      '2026-10-20T00:14:59.000Z').fresh, true);
    assert.equal(readDeviceStateAck(db, deviceId,
      '2026-10-20T00:16:00.000Z').fresh, false);
  } finally {
    db.close();
    rmSync(dir, { recursive: true, force: true });
  }
});
