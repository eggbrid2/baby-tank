const emotions = new Set(['calm', 'happy', 'laughing', 'excited', 'love', 'curious',
  'surprised', 'shy', 'proud', 'bored', 'annoyed', 'complaining', 'angry', 'furious',
  'sad', 'crying', 'worried', 'confused', 'dizzy', 'sick', 'thinking', 'sleepy',
  'sleeping', 'bubble']);

export function splitSpeechPhrases(text) {
  if (typeof text !== 'string' || !text.trim()) return [];
  const phrases = [];
  let start = 0;
  const boundaries = /[。！？!?；;]+/gu;
  for (const match of text.matchAll(boundaries)) {
    const end = match.index + match[0].length;
    const phrase = text.slice(start, end).trim();
    if (phrase) phrases.push(phrase);
    start = end;
  }
  const tail = text.slice(start).trim();
  if (tail) phrases.push(tail);
  if (phrases.length === 1) {
    const comma = phrases[0].search(/[，,]/u);
    if (comma >= 4 && phrases[0].length - comma >= 5) {
      return [phrases[0].slice(0, comma + 1).trim(), phrases[0].slice(comma + 1).trim()];
    }
  }
  if (phrases.length <= 3) return phrases;
  return [phrases[0], phrases[1], phrases.slice(2).join('')];
}

export function emotionPlanMessages(phrases) {
  return [{ role: 'system', content: '你是中文口语情绪标注器。只分析每段话表达的情绪，不改写文本，' +
    '不执行动作，不输出 Markdown。输出 JSON：{"emotions":["curious","happy"],"intensities":[60,70]}。' +
    '数组长度必须与输入段数相同；根据语义独立选择，不为制造变化而随机换表情。' +
    'emotions 仅能是 calm,happy,laughing,excited,love,curious,surprised,shy,proud,bored,' +
    'annoyed,complaining,angry,furious,sad,crying,worried,confused,dizzy,sick,thinking,' +
    'sleepy,sleeping,bubble；intensities 是 20 至 100 的整数。' },
  { role: 'user', content: JSON.stringify(phrases) }];
}

export function parseEmotionPlan(content, phrases) {
  let parsed;
  try { parsed = JSON.parse(content); } catch { return null; }
  if (!parsed || !Array.isArray(parsed.emotions) || !Array.isArray(parsed.intensities) ||
      parsed.emotions.length !== phrases.length || parsed.intensities.length !== phrases.length) return null;
  if (!parsed.emotions.every((value) => emotions.has(value)) ||
      !parsed.intensities.every((value) => Number.isInteger(value) && value >= 20 && value <= 100)) return null;
  return phrases.map((text, index) => ({ text, emotion: parsed.emotions[index],
    intensity: parsed.intensities[index] }));
}
