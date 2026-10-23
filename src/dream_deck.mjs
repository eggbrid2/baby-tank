import { createHash } from 'node:crypto';

// The model chooses and orders safe dream cards; it never supplies pixels or
// arbitrary glyphs.  Artwork is rendered locally by the head, so a dream deck
// remains small, deterministic and usable offline.
export const DREAM_CATALOG = Object.freeze([
  ['star-1', 'star', '亮晶晶'], ['star-2', 'star', '晶晶亮'],
  ['moon-1', 'moon', '做个好梦'], ['moon-2', 'moon', '好梦'],
  ['cloud-1', 'cloud', '软绵绵'], ['cloud-2', 'cloud', '软软'],
  ['heart-1', 'heart', '想抱抱'], ['heart-2', 'heart', '抱一抱'],
  ['flower-1', 'flower', '花开啦'], ['flower-2', 'flower', '花开'],
  ['rocket-1', 'rocket', '去探险'], ['rocket-2', 'rocket', '出去玩'],
  ['car-1', 'car', '出去玩'], ['car-2', 'car', '去玩'],
  ['ball-1', 'ball', '滚一滚'], ['ball-2', 'ball', '玩一玩'],
  ['fish-1', 'fish', '小鱼游'], ['fish-2', 'fish', '鱼游'],
  ['bubble-1', 'bubble', '吹泡泡'], ['bubble-2', 'bubble', '泡泡'],
  ['question-1', 'question', '想一想'], ['question-2', 'question', '想想'],
  ['music-1', 'music', '哼小曲'], ['music-2', 'music', '哼曲'],
  ['bird-1', 'bird', '出去玩'], ['bird-2', 'bird', '玩一玩'],
  ['book-1', 'book', '想一想'], ['book-2', 'book', '做个好梦'],
  ['kite-1', 'kite', '去探险'], ['kite-2', 'kite', '吹一吹'],
].map(([id, key, displayText]) => Object.freeze({ id, key, displayText })));

const byId = new Map(DREAM_CATALOG.map((entry) => [entry.id, entry]));

function seededOrder(seed) {
  const digest = createHash('sha256').update(String(seed)).digest();
  const result = [...DREAM_CATALOG];
  let cursor = 0;
  for (let index = result.length - 1; index > 0; --index) {
    const swap = digest[cursor++ % digest.length] % (index + 1);
    [result[index], result[swap]] = [result[swap], result[index]];
  }
  return result;
}

export function fallbackDreamDeck(seed = 'baby-tank') {
  return seededOrder(seed).map(({ key, displayText }) => ({ key, displayText }));
}

export function dreamDeckPrompt(mood, band) {
  return [
    { role: 'system', content:
      '你为儿童陪伴机器人安排睡前梦境卡组。只从候选编号中选择，输出严格 JSON：' +
      '{"ids":[30个编号]}。每个编号必须恰好出现一次，不要输出解释。' +
      '排列要有起伏，相邻卡片尽量避免同类图形；内容温柔、安全，不包含惊吓、战斗或购买。' },
    { role: 'user', content: JSON.stringify({ mood, band,
      candidates: DREAM_CATALOG.map(({ id, key, displayText }) =>
        ({ id, key, displayText })) }) },
  ];
}

export function tryParseDreamDeckReply(content) {
  let value;
  try {
    const text = String(content || '').trim();
    const start = text.indexOf('{');
    const end = text.lastIndexOf('}');
    value = JSON.parse(start >= 0 && end >= start ? text.slice(start, end + 1) : text);
  } catch {
    return null;
  }
  if (!Array.isArray(value?.ids) || value.ids.length !== DREAM_CATALOG.length ||
      new Set(value.ids).size !== DREAM_CATALOG.length ||
      value.ids.some((id) => !byId.has(id))) {
    return null;
  }
  return value.ids.map((id) => {
    const { key, displayText } = byId.get(id);
    return { key, displayText };
  });
}

export function parseDreamDeckReply(content, fallbackSeed = 'baby-tank') {
  return tryParseDreamDeckReply(content) || fallbackDreamDeck(fallbackSeed);
}
