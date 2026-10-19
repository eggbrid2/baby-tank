import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openDatabase } from '../src/db.mjs';
import { createSecretBox } from '../src/lib.mjs';
import { readConversationMemory, saveConversationMemory } from '../src/conversation_memory.mjs';

test('encrypted per-device conversation context survives cloud restart and stays bounded', (t) => {
  const directory = mkdtempSync(join(tmpdir(), 'tank-memory-'));
  const path = join(directory, 'tank.sqlite3');
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  const box = createSecretBox(Buffer.alloc(32, 7));
  const bootstrap = { username: 'admin', password: 'correct-horse-battery-staple' };
  let db = openDatabase(path, bootstrap);
  db.prepare(`INSERT INTO devices(device_id,name,board,token_hash,created_at)
    VALUES (?,?,?,?,?)`).run('tank-1', 'Tank', 'head', 'token-hash', new Date().toISOString());
  const deviceId = db.prepare('SELECT id FROM devices WHERE device_id=?').get('tank-1').id;
  const base = Date.now();
  for (let index = 0; index < 12; ++index) {
    saveConversationMemory(db, box, deviceId, `private-user-${index}`,
      `private-answer-${index}`, base + index);
  }
  assert.equal(db.prepare('SELECT COUNT(*) AS count FROM conversation_turns').get().count, 8);
  db.close();
  assert.equal(readFileSync(path).includes(Buffer.from('private-user-11')), false);

  db = openDatabase(path, bootstrap);
  const remembered = readConversationMemory(db, box, deviceId, base + 12);
  assert.equal(remembered.length, 8);
  assert.equal(remembered[0].user, 'private-user-4');
  assert.equal(remembered.at(-1).assistant, 'private-answer-11');
  assert.deepEqual(readConversationMemory(db, box, deviceId,
    base + 31 * 24 * 60 * 60 * 1000), []);
  db.close();
});
