// Execute inside the cloud container with MASTER_KEY and its encrypted DB.
// Only generated PCM is copied out; provider credentials remain in the container.
import { DatabaseSync } from 'node:sqlite';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createSecretBox } from '../src/lib.mjs';
import { synthesizeWithVolcengine } from '../src/adapters.mjs';

const key = Buffer.from(process.env.MASTER_KEY || '', 'base64');
if (key.length !== 32) throw new Error('MASTER_KEY unavailable');
const dataDir = resolve(process.env.DATA_DIR || '/data');
const outputDir = resolve(process.env.OUTPUT_DIR || '/tmp');
mkdirSync(outputDir, { recursive: true });
const db = new DatabaseSync(resolve(dataDir, 'baby-tank.sqlite3'));
const selected = db.prepare(`SELECT * FROM provider_configs
  WHERE kind='tts' AND enabled=1 ORDER BY updated_at DESC LIMIT 1`).get();
if (!selected) throw new Error('TTS provider unavailable');
const credentials = JSON.parse(createSecretBox(key).decrypt(selected.secret_ciphertext));
selected.options_json = JSON.stringify({ ...JSON.parse(selected.options_json || '{}'),
  audioFormat: 'pcm', sampleRate: '16000', speechRate: 15 });

const phrases = [
  ['huhu', '呼呼。'],
  ['dont-touch', '哎呀，别摸我。'],
  ['getting-angry', '再摸我，我生气了！'],
  ['stop-touching', '别摸了！'],
  ['left-ticklish', '好痒呀！'],
  ['left-again', '嘿嘿，再来一下。'],
  ['left-found-me', '被你发现啦！'],
  ['left-itchy', '左边有点痒！'],
  ['left-comfy', '好舒服呀。'],
  ['left-like-you', '我也喜欢你。'],
  ['right-who', '咦，谁碰我？'],
  ['right-not-startled', '哼，才没吓到我。'],
  ['right-too', '这边也要摸吗？'],
  ['right-again', '你又来啦。'],
  ['right-one-more', '好吧，再摸一下。'],
  ['right-watching', '我盯着你呢。'],
  ['rare-sneeze', '阿嚏！鼻子好痒！'],
  ['rare-catch-me', '嘿嘿，抓不到我！'],
  ['rare-easter-egg', '彩蛋被你找到啦！'],
  ['rare-transform', '咕噜，变身啦！'],
];
for (const [name, text] of phrases) {
  const result = await synthesizeWithVolcengine({ selected, credentials,
    text, deviceId: 'touch-clip-generation' });
  if (result.format !== 'pcm' || result.sampleRate !== 16000) {
    throw new Error(`PCM16k required: ${name}`);
  }
  const chunks = [];
  for await (const chunk of result.audio) chunks.push(chunk);
  const pcm = Buffer.concat(chunks);
  if (pcm.length < 3200 || pcm.length > 128000 || pcm.length % 2 !== 0) {
    throw new Error(`Unexpected clip size for ${name}: ${pcm.length}`);
  }
  writeFileSync(resolve(outputDir, `baby-tank-touch-${name}.pcm`), pcm);
  process.stdout.write(`${name}: ${pcm.length} bytes, ${Math.round(pcm.length / 32)} ms\n`);
}
