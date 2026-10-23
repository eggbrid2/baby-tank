import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openDatabase } from '../src/db.mjs';
import { authenticateParent, claimParentAccess, createParentClaim,
  revokeDeviceParents, revokeParentAccess } from '../src/parent_access.mjs';

test('one-time pairing creates only a device-scoped, revocable parent session', (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'baby-tank-parent-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const db = openDatabase(join(dir, 'db.sqlite3'),
    { username: 'admin', password: 'test-password-long-enough' });
  t.after(() => db.close());
  const at = new Date('2026-10-20T04:00:00.000Z');
  const deviceId = Number(db.prepare(`INSERT INTO devices(device_id,name,board,
    token_hash,created_at) VALUES (?,?,?,?,?)`).run('tank-one', 'one', 's3',
    'hash1', at.toISOString()).lastInsertRowid);
  const claim = createParentClaim(db, deviceId, at);
  assert.match(claim.code, /^[0-9A-F]{4}(?:-[0-9A-F]{4}){3}$/);
  assert.equal(claimParentAccess(db, '0000-0000-0000-0000', at), null);
  const access = claimParentAccess(db, claim.code.toLowerCase(), at);
  assert.equal(access.deviceId, 'tank-one');
  assert.equal(claimParentAccess(db, claim.code, at), null, 'claim is one-time');
  assert.equal(authenticateParent(db, `Bearer ${access.token}`, at).device_id,
    deviceId);
  assert.equal(authenticateParent(db, `Bearer ${access.token}`,
    new Date('2026-11-20T04:00:00.000Z')), null);
  assert.equal(revokeParentAccess(db, `Bearer ${access.token}`, at), true);
  assert.equal(authenticateParent(db, `Bearer ${access.token}`, at), null);
  const second = createParentClaim(db, deviceId, at);
  const secondAccess = claimParentAccess(db, second.code, at);
  assert.equal(revokeDeviceParents(db, deviceId, at), 1);
  assert.equal(authenticateParent(db, `Bearer ${secondAccess.token}`, at), null);
});
