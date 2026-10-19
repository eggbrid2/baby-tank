// Run inside the cloud container with MASTER_KEY and its encrypted provider DB.
// Credentials never leave that container; only short PCM clips are exported.
import { DatabaseSync } from 'node:sqlite';
import { writeFileSync } from 'node:fs';
import { createSecretBox } from './src/lib.mjs';
import { synthesizeWithVolcengine } from './src/adapters.mjs';

const key = Buffer.from(process.env.MASTER_KEY || '', 'base64');
if (key.length !== 32) throw new Error('MASTER_KEY unavailable');
const db = new DatabaseSync('/data/baby-tank.sqlite3');
const selected = db.prepare('SELECT * FROM provider_configs WHERE kind=? AND enabled=1 ORDER BY updated_at DESC LIMIT 1').get('tts');
if (!selected) throw new Error('TTS provider unavailable');
const credentials = JSON.parse(createSecretBox(key).decrypt(selected.secret_ciphertext));
const options = JSON.parse(selected.options_json || '{}');
selected.options_json = JSON.stringify({ ...options, audioFormat: 'pcm', sampleRate: '16000' });

const phrases = [
  ['wozai', '我在呢！'],
  ['lailao', '来啦！'],
  ['remote-mode', '已进入遥控模式。'],
  ['please-move', '让一让！'],
  ['coming-through', '我想过一下。'],
];
const requested = new Set(process.argv.slice(2));
for (const [name, words] of phrases) {
  if (requested.size > 0 && !requested.has(name)) continue;
  const result = await synthesizeWithVolcengine({ selected, credentials,
    text: words, deviceId: 'wake-clip-generation' });
  if (result.format !== 'pcm' || result.sampleRate !== 16000) throw new Error('PCM16k required');
  const chunks = [];
  for await (const chunk of result.audio) chunks.push(chunk);
  const pcm = Buffer.concat(chunks);
  if (pcm.length < 3200 || pcm.length > 128000 || pcm.length % 2 !== 0) {
    throw new Error(`Unexpected clip size for ${name}: ${pcm.length}`);
  }
  writeFileSync(`/tmp/baby-tank-wake-${name}.pcm`, pcm);
  process.stdout.write(`${name}: ${pcm.length} bytes, ${Math.round(pcm.length / 32)} ms\n`);
}
