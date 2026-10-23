import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openDatabase } from '../src/db.mjs';
import { createSecretBox } from '../src/lib.mjs';
import { archiveQuestion, deleteQuestion, listQuestions, pruneExpiredQuestions,
  readQuestionHistoryPolicy, setQuestionHistoryPolicy } from '../src/question_history.mjs';

test('question archive is off by default, encrypted, paginated and erased on opt-out', (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'baby-tank-questions-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const path = join(dir, 'db.sqlite3');
  const db = openDatabase(path, { username: 'admin',
    password: 'test-password-long-enough' });
  t.after(() => db.close());
  const box = createSecretBox(Buffer.alloc(32, 9));
  const at = '2026-10-20T04:00:00.000Z';
  const deviceId = Number(db.prepare(`INSERT INTO devices(device_id,name,board,
    token_hash,created_at) VALUES (?,?,?,?,?)`).run('test-device', 'test', 's3',
    'hash', at).lastInsertRowid);
  assert.equal(readQuestionHistoryPolicy(db, deviceId).enabled, false);
  assert.equal(archiveQuestion(db, box, deviceId, 'trace-1',
    '孩子的隐私提问', '一个回答', at), false);
  setQuestionHistoryPolicy(db, deviceId, { enabled: true, retentionDays: 7 }, at);
  assert.equal(archiveQuestion(db, box, deviceId, 'trace-1',
    '孩子的隐私提问', '一个回答', at), true);
  assert.equal(archiveQuestion(db, box, deviceId, 'trace-1',
    '孩子的隐私提问', '一个回答', at), true);
  const page = listQuestions(db, box, deviceId, {}, at);
  assert.equal(page.length, 1, 'trace id deduplicates retries');
  assert.equal(page[0].question, '孩子的隐私提问');
  assert.equal(readFileSync(path).includes(Buffer.from('孩子的隐私提问')), false);
  assert.deepEqual(listQuestions(db, box, deviceId, { beforeId: page[0].id }, at), []);
  assert.equal(deleteQuestion(db, deviceId, page[0].id), 1);
  assert.deepEqual(listQuestions(db, box, deviceId, {}, at), []);
  archiveQuestion(db, box, deviceId, 'trace-2', '另一个问题', '另一个回答', at);
  assert.equal(pruneExpiredQuestions(db, '2026-10-29T04:00:00.000Z'), 1);
  assert.deepEqual(listQuestions(db, box, deviceId, {},
    '2026-10-29T04:00:00.000Z'), []);
  archiveQuestion(db, box, deviceId, 'trace-3', '最后的问题', '最后的回答', at);
  setQuestionHistoryPolicy(db, deviceId, { enabled: false, retentionDays: 7 }, at);
  assert.deepEqual(listQuestions(db, box, deviceId, {}, at), []);
  assert.throws(() => setQuestionHistoryPolicy(db, deviceId,
    { enabled: true, retentionDays: 365 }, at), /invalid_question_history_policy/);
});
