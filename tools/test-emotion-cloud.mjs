// Run inside the cloud container; prints only synthetic test metadata.
import { DatabaseSync } from 'node:sqlite';
import { createSecretBox } from '../src/lib.mjs';
import { requestChatCompletion } from '../src/adapters.mjs';
import { conversationMessages, parseConversationReply } from '../src/conversation.mjs';

const key = Buffer.from(process.env.MASTER_KEY || '', 'base64');
if (key.length !== 32) throw new Error('MASTER_KEY unavailable');
const db = new DatabaseSync('/data/baby-tank.sqlite3');
const selected = db.prepare('SELECT * FROM provider_configs WHERE kind=? AND enabled=1 ORDER BY updated_at DESC LIMIT 1').get('llm');
if (!selected) throw new Error('LLM unavailable');
const credentials = JSON.parse(createSecretBox(key).decrypt(selected.secret_ciphertext));
const started = Date.now();
const response = await requestChatCompletion({ selected, credentials,
  request: { messages: conversationMessages('先惊讶地说你发现一只小猫，再开心地说你想和它打招呼。', [], {}),
    stream: false, temperature: 0.7, max_tokens: 400 } });
if (!response.ok) throw new Error(`LLM HTTP ${response.status}`);
const body = await response.json();
const reply = parseConversationReply(body?.choices?.[0]?.message?.content);
if (!reply) throw new Error('Invalid LLM reply');
process.stdout.write(JSON.stringify({ durationMs: Date.now() - started,
  replyCharacters: reply.text.length, modelSegmentSource: reply.speechSegmentSource,
  segmentCount: reply.speechSegments.length, planned: false,
  emotionTimeline: reply.speechSegments.map(({ emotion }) => emotion) }) + '\n');
