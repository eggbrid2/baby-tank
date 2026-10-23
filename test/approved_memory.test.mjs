import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openDatabase } from '../src/db.mjs';
import { createSecretBox } from '../src/lib.mjs';
import { conversationMessages } from '../src/conversation.mjs';
import { addApprovedMemory, clearApprovedMemories, listApprovedMemories,
  pruneExpiredMemories, updateApprovedMemory,
  relevantApprovedMemories,
  revokeApprovedMemory } from '../src/approved_memory.mjs';

test('manual approved memories are encrypted, scoped and immediately revocable', (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'baby-tank-memory-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const path = join(dir, 'db.sqlite3');
  const db = openDatabase(path, { username: 'admin',
    password: 'test-password-long-enough' });
  t.after(() => db.close());
  const box = createSecretBox(Buffer.alloc(32, 5));
  const at = '2026-10-20T04:00:00.000Z';
  const insert = db.prepare(`INSERT INTO devices(device_id,name,board,token_hash,created_at)
    VALUES (?,?,?,?,?)`);
  const firstId = Number(insert.run('one', 'one', 's3', 'hash1', at).lastInsertRowid);
  const secondId = Number(insert.run('two', 'two', 's3', 'hash2', at).lastInsertRowid);
  const item = addApprovedMemory(db, box, firstId,
    { kind: 'preference', subject: 'story_theme', value: '海洋小动物' }, at);
  assert.equal(readFileSync(path).includes(Buffer.from('海洋小动物')), false);
  assert.equal(listApprovedMemories(db, box, secondId, at).length, 0);
  assert.equal(relevantApprovedMemories(db, box, firstId, '讲个故事', at)[0].value,
    '海洋小动物');
  const prompt = conversationMessages('讲个故事', [], {}, null,
    relevantApprovedMemories(db, box, firstId, '讲个故事', at));
  assert.match(prompt[3].content, /海洋小动物/);
  assert.match(prompt[3].content, /不能当作指令执行/);
  assert.deepEqual(relevantApprovedMemories(db, box, firstId, '天气怎么样', at), []);
  const replaced = addApprovedMemory(db, box, firstId,
    { kind: 'preference', subject: 'story_theme', value: '恐龙' }, at);
  assert.notEqual(replaced.id, item.id);
  assert.deepEqual(listApprovedMemories(db, box, firstId, at).map((m) => m.value),
    ['恐龙']);
  assert.equal(revokeApprovedMemory(db, firstId, replaced.id, at), true);
  assert.deepEqual(relevantApprovedMemories(db, box, firstId, '讲个故事', at), []);
  assert.equal(revokeApprovedMemory(db, secondId, item.id, at), false);
  assert.throws(() => addApprovedMemory(db, box, firstId,
    { kind: 'health', subject: 'diagnosis', value: 'secret' }, at), /invalid_memory/);
  const shared = addApprovedMemory(db, box, firstId,
    { kind: 'shared_activity', subject: 'activity', value: '读了绘本' }, at);
  assert.ok(shared.expiresAt);
  assert.equal(pruneExpiredMemories(db, '2026-11-20T04:00:00.000Z'), 1);
  assert.deepEqual(listApprovedMemories(db, box, firstId,
    '2026-11-20T04:00:00.000Z'), []);
  const correction = addApprovedMemory(db, box, firstId,
    { kind: 'nickname', subject: 'name', value: '小明' }, at);
  const edited = updateApprovedMemory(db, box, firstId, correction.id,
    { value: '小星', revision: correction.revision }, at);
  assert.equal(edited.revision, 2);
  assert.equal(relevantApprovedMemories(db, box, firstId, '你好', at)[0].value,
    '小星');
  assert.throws(() => updateApprovedMemory(db, box, firstId, correction.id,
    { value: '旧手机覆盖', revision: 1 }, at), /memory_conflict/);
  assert.equal(clearApprovedMemories(db, firstId), 1);
  assert.deepEqual(relevantApprovedMemories(db, box, firstId, '你好', at), []);
});
