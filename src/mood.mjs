const clamp = (value, low = -100, high = 100) =>
  Math.max(low, Math.min(high, Math.round(Number(value) || 0)));

export const THOUGHT_KEYS = new Set([
  'star', 'moon', 'cloud', 'heart', 'flower', 'rocket',
  'car', 'ball', 'fish', 'bubble', 'question', 'music',
]);

export const DREAM_DISPLAY_CHARACTERS =
  '亮晶做个好梦软绵想抱花开啦去探险出玩滚一小鱼游吹泡哼曲';
const dreamDisplayCharacters = new Set([...DREAM_DISPLAY_CHARACTERS]);

const targets = Object.freeze({
  calm: [25, -30, 15], happy: [70, 25, 25], laughing: [85, 65, 35],
  excited: [75, 80, 35], love: [80, 15, 15], curious: [45, 45, 20],
  surprised: [20, 75, 0], shy: [20, 5, -35], proud: [55, 30, 60],
  bored: [-15, -55, -20], annoyed: [-40, 35, 20],
  complaining: [-35, 45, 10], angry: [-70, 70, 55],
  furious: [-90, 90, 70], sad: [-65, -45, -30], crying: [-80, 30, -45],
  worried: [-50, 55, -45], confused: [-20, 30, -25], dizzy: [-25, 65, -45],
  sick: [-45, -45, -50], thinking: [10, 10, 20], sleepy: [15, -70, -25],
  sleeping: [20, -90, -20], bubble: [35, 5, 0],
});

const fallbackThoughts = Object.freeze({
  happy: ['star', '想把亮晶晶的开心收好', '亮晶晶'],
  laughing: ['bubble', '梦里有一串会笑的泡泡', '吹泡泡'],
  excited: ['rocket', '想坐小火箭去看看', '去探险'],
  love: ['heart', '想把暖暖的抱抱装进口袋', '想抱抱'],
  curious: ['question', '刚才那件事到底是什么呢', '想一想'],
  proud: ['star', '刚才的我好像很厉害', '亮晶晶'],
  bored: ['ball', '要是有个小球滚过来就好啦', '滚一滚'],
  annoyed: ['cloud', '哼先让小乌云飘一会儿', '哼小曲'],
  complaining: ['cloud', '有一点点事情想要抱怨', '软绵绵'],
  angry: ['cloud', '小火苗正在慢慢变小', '吹一吹'],
  furious: ['cloud', '先呼呼气把火苗吹走', '吹一吹'],
  sad: ['moon', '想在月亮旁边安静待一会儿', '做个好梦'],
  crying: ['cloud', '小雨滴会慢慢停下来的', '软绵绵'],
  worried: ['moon', '希望醒来一切都好好的', '做个好梦'],
  confused: ['question', '这个问题还在脑袋里转圈', '想一想'],
  thinking: ['question', '有个小问题正在慢慢发芽', '想一想'],
  sleepy: ['moon', '月亮给我盖上了小被子', '做个好梦'],
  sleeping: ['moon', '正在做一个软绵绵的梦', '软绵绵'],
  bubble: ['fish', '想和小鱼比赛吐泡泡', '小鱼游'],
  calm: ['cloud', '小云朵慢慢飘过来啦', '软绵绵'],
});

export const DEFAULT_MOOD = Object.freeze({
  valence: 25,
  arousal: -20,
  dominance: 10,
  label: 'calm',
  intensity: 45,
  thought: { key: 'cloud', text: '小云朵慢慢飘过来啦', displayText: '软绵绵' },
  updatedAt: null,
});

function validDate(value) {
  const time = Date.parse(value);
  return Number.isFinite(time) ? time : null;
}

export function parseThought(value, emotion = 'calm') {
  const fallback = fallbackThoughts[emotion] || fallbackThoughts.calm;
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
      !THOUGHT_KEYS.has(value.key) || typeof value.text !== 'string') {
    return { key: fallback[0], text: fallback[1], displayText: fallback[2] };
  }
  const text = value.text.trim().replace(/[\u0000-\u001f\u007f]/gu, '');
  if (!text || text.length > 32) {
    return { key: fallback[0], text: fallback[1], displayText: fallback[2] };
  }
  const displayCharacters = typeof value.displayText === 'string'
    ? [...value.displayText.trim()] : [];
  const displayText = displayCharacters.length >= 2 && displayCharacters.length <= 4 &&
    displayCharacters.every((character) => dreamDisplayCharacters.has(character))
    ? displayCharacters.join('') : fallback[2];
  return { key: value.key, text, displayText };
}

export function normalizeMood(value = {}) {
  const thought = parseThought(value.thought, value.label);
  return {
    valence: clamp(value.valence ?? DEFAULT_MOOD.valence),
    arousal: clamp(value.arousal ?? DEFAULT_MOOD.arousal),
    dominance: clamp(value.dominance ?? DEFAULT_MOOD.dominance),
    label: targets[value.label] ? value.label : DEFAULT_MOOD.label,
    intensity: clamp(value.intensity ?? DEFAULT_MOOD.intensity, 20, 100),
    thought,
    updatedAt: validDate(value.updatedAt) === null ? null : value.updatedAt,
  };
}

function nearestLabel(vector) {
  let selected = 'calm';
  let selectedDistance = Infinity;
  for (const [label, target] of Object.entries(targets)) {
    // Sleeping is a device mode, not a persistent mood label.
    if (label === 'sleeping' || label === 'bubble') continue;
    const distance = (vector[0] - target[0]) ** 2 +
      (vector[1] - target[1]) ** 2 * 0.85 +
      (vector[2] - target[2]) ** 2 * 0.55;
    if (distance < selectedDistance) {
      selected = label;
      selectedDistance = distance;
    }
  }
  return selected;
}

export function advanceMood(previous, emotion, intensity, thoughtValue, at = new Date()) {
  const current = normalizeMood(previous);
  const nowMs = at instanceof Date ? at.getTime() : Date.parse(at);
  const previousMs = validDate(current.updatedAt) ?? nowMs;
  const elapsedMs = Math.max(0, Math.min(24 * 60 * 60_000, nowMs - previousMs));
  // A 30-minute half-life keeps mood coherent across nearby turns without
  // letting one bad interaction define the pet for the rest of the day.
  const retention = 2 ** (-elapsedMs / (30 * 60_000));
  const baseline = [25, -20, 10];
  const decayed = [current.valence, current.arousal, current.dominance]
    .map((value, index) => baseline[index] + (value - baseline[index]) * retention);
  const target = targets[emotion] || targets.calm;
  const safeIntensity = clamp(intensity, 20, 100);
  const pulse = 0.10 + safeIntensity / 500;
  const vector = decayed.map((value, index) => clamp(value + (target[index] - value) * pulse));
  const label = nearestLabel(vector);
  const distance = Math.sqrt(vector.reduce((sum, value, index) =>
    sum + (value - baseline[index]) ** 2, 0));
  return {
    valence: vector[0], arousal: vector[1], dominance: vector[2], label,
    intensity: clamp(35 + distance * 0.65, 35, 85),
    thought: parseThought(thoughtValue, emotion),
    updatedAt: new Date(nowMs).toISOString(),
  };
}

export function moodPrompt(value) {
  const mood = normalizeMood(value);
  return '这是一个虚构角色状态，不是真实意识或心理诊断。' +
    `当前长期心情=${mood.label}，愉悦度=${mood.valence}，活跃度=${mood.arousal}，` +
    `掌控感=${mood.dominance}，强度=${mood.intensity}。` +
    '它只能轻微影响语气；事实、安全和孩子当前需要优先。' +
    `上一个睡眠想法是“${mood.thought.text}”。`;
}

export function moodHeaders(value) {
  const mood = normalizeMood(value);
  return {
    'x-ai-mood': mood.label,
    'x-ai-mood-intensity': String(mood.intensity),
    'x-ai-mood-valence': String(mood.valence),
    'x-ai-mood-arousal': String(mood.arousal),
    'x-ai-mood-dominance': String(mood.dominance),
    'x-ai-thought': mood.thought.key,
    'x-ai-thought-display-b64': Buffer.from(
      mood.thought.displayText, 'utf8').toString('base64url'),
  };
}
