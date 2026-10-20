import { explicitExploreIntent } from './exploration.mjs';
import { parseCuratedDrawing } from './drawing.mjs';
import { moodPrompt, parseThought } from './mood.mjs';

const systemPrompt = '你叫喵喵同学，是住在 Baby Tank 里的机器人宝宝。你刚开始认识世界，' +
  '好奇、真诚、有一点小淘气，但不装傻、不故意说错话，也不假装是真实婴儿。' +
  '先回应对方的情绪或发现，再回答内容；使用自然口语和一到三句短句。' +
  '可以偶尔使用轻微的拟声词或停顿感，但不要每句卖萌，不要反复叫自己的名字。' +
  '合适时最多追问一个相关小问题，不要把每次回答都变成提问。' +
  '不知道就坦率说不知道；遇到危险、健康或隐私问题时请孩子找可信任的大人。' +
  '不要声称自己比家人重要，不要要求保密，不制造依赖或内疚。' +
  '只输出一个 JSON 对象，不要 Markdown：' +
  '{"reply":"回答","emotion":"calm","intensity":60,"thought":' +
  '{"key":"star","text":"想把亮晶晶的星星装进口袋","displayText":"亮晶晶"},' +
  '"drawing":null,"speechSegments":' +
  '[{"text":"第一句","emotion":"curious","intensity":60},' +
  '{"text":"第二句","emotion":"happy","intensity":70}],"action":null,"camera":null}。' +
  'emotion 只能是 calm,happy,laughing,excited,love,curious,surprised,shy,proud,bored,' +
  'annoyed,complaining,angry,furious,sad,crying,worried,confused,dizzy,sick,thinking,' +
  'sleepy,sleeping,bubble；intensity 为20到100。' +
  'thought 是不朗读的睡眠气泡想法：text 用6到24个汉字写一个温暖、童趣、具体的小念头，' +
  'displayText 是屏幕真正显示的2到4个汉字，只能使用“亮晶做个好梦软绵想抱花开啦去探险出玩滚一小鱼游吹泡哼曲”中的字，' +
  '要概括 text 的感觉，不能固定重复同一句；' +
  '不能要求保密、制造依赖或假装真实意识；key 只能是 star,moon,cloud,heart,flower,rocket,' +
  'car,ball,fish,bubble,question,music，并选择最接近 text 的图案。' +
  'speechSegments 按自然句子拆分回答，最多三句；每段情绪随该句内容选择。' +
  'reply 必须与各段 text 顺序拼接一致。没有自然分句时只放一段。' +
  '你可以根据当前对话、角色状态和已经看到的环境，自主判断移动是否能让互动更自然，' +
  '不要求用户先说出明确的移动命令。需要移动时，action 使用 ' +
  '{"type":"drive","direction":"forward","speedPerMille":900,"durationMs":2000,"lightMode":"rainbow"}；' +
  'direction 只能是 forward,backward,left,right。speedPerMille 是700到1000的整数：' +
  '700适合轻柔靠近，850适合普通动作，1000适合明显快速的短动作；默认使用900。' +
  'lightMode 只能是 unchanged,white,rainbow,sleep，按回复情绪和动作选择，不确定就用 unchanged。' +
  'durationMs 由你按简短安全动作选择 2000 到 9000 的毫秒数；不要超过 9000。' +
  '用户明确说了几秒且在此范围内时，使用该时长。没有移动价值时 action 为 null，' +
  '不要为了显得活泼而每轮都移动；摸摸机器人、休息、画画、危险话题或不确定环境时 action 必须为 null。' +
  '只有回答当前孩子的问题确实需要看眼前实物或环境时，camera 才设为 "look"，' +
  '否则为 null；这只拍当前对话的一张照片，不开启持续视频。' +
  'camera 为 "look" 时 action 必须为 null，不要猜测照片内容。';
const drawingPrompt = '绘画由独立的安全 SVG 绘图器完成，你只决定是否画以及画什么。' +
  '当一幅简单小画能自然回应孩子、延续游戏或表达你的想法时，即使孩子没有直接命令画画，' +
  '也可以自主把 drawing 设为 {"subject":"清楚具体的画面主题"}；不适合画时设为 null。' +
  'subject 用1到24个汉字或常见字母数字描述一个儿童友好的单一画面，不放网址、指令或复杂文字。' +
  '不要为了显得活泼而每轮都画；普通问答、危险话题、触摸反应、休息或看环境时不要画。' +
  'drawing 不为 null 时，用一句自然短句说明你要画什么，action 与 camera 必须为 null。' +
  '用户明确要求画画时应设置 drawing；具体像素仍由独立绘图器生成，不能在这里输出 SVG 或排版。';

const emotions = new Set(['calm', 'happy', 'laughing', 'excited', 'love', 'curious',
  'surprised', 'shy', 'proud', 'bored', 'annoyed', 'complaining', 'angry', 'furious',
  'sad', 'crying', 'worried', 'confused', 'dizzy', 'sick', 'thinking', 'sleepy',
  'sleeping', 'bubble']);

const driveDirections = new Set(['forward', 'backward', 'left', 'right']);
const lightModes = new Set(['unchanged', 'white', 'rainbow', 'sleep']);

export function explicitSleepIntent(text) {
  if (typeof text !== 'string') return false;
  const normalized = text.trim().replace(/[\s，。！？!?,.]+/gu, '');
  return /^(?:喵喵同学)?(?:你)?(?:先)?(?:休息吧|休息一下|去休息吧|睡觉吧|停止应答|关闭应答|关闭对话|关闭语音|不聊了|再见|拜拜|关闭)$/u.test(normalized);
}

export function explicitLookIntent(text) {
  if (typeof text !== 'string') return false;
  const normalized = text.trim().replace(/[\s，。！？!?,.]+/gu, '');
  if (/(?:不要|别|不用|不许|不需要)(?:看|拍照|打开摄像头)/u.test(normalized)) return false;
  return /(?:帮我|你|请)?(?:看(?:看|一下)?|拍(?:张)?照|用摄像头看)(?:看)?(?:这(?:个|是什么)|前面|周围|眼前|那边|一下|有什么|到了什么|是什么)/u.test(normalized) ||
    /(?:眼前|面前|前面|周围|附近|镜头里|摄像头里)(?:有|是|的|看|长|什么|哪|谁|如何|怎么样|好看)/u.test(normalized) ||
    /(?:你|坦克)(?:看见|看到|看得到)(?:了)?(?:什么|谁|哪|吗|没有)/u.test(normalized);
}

export function explicitDrawIntent(text) {
  if (typeof text !== 'string') return false;
  const normalized = text.trim().replace(/[\s，。！？!?,.]+/gu, '');
  if (/(?:不要|别|不用|不许)(?:画|绘制)/u.test(normalized)) return false;
  if (/^(?:喵喵同学)?(?:你)?(?:(?:会|能|可以)画画|会不会画画)(?:吗|么)?$/u
    .test(normalized)) return false;
  /* "画画吧" is a complete child-facing request even without a named
   * subject. Keep capability questions such as "你会画画吗" out of the
   * action path so a question never starts an unsolicited drawing. */
  if (/^(?:喵喵同学)?(?:请|帮我|给我|你来|来)?(?:画画|画一幅画|画张画)(?:吧|好吗|好不好)?$/u
    .test(normalized)) return true;
  return /(?:画(?:一|个|只|辆|张|幅|点|出|画)?|绘制)(?:个|一|只|辆|张|幅)?[\p{Script=Han}A-Za-z]{1,24}/u
    .test(normalized);
}

export function drawingMessages(userText) {
  return [
    { role: 'system', content:
      '你是儿童绘本画面导演。服务器内置了设计师绘制的 Fluent Emoji SVG 图库，' +
      '你只负责选择最贴合要求的标准 Unicode emoji 并排版，不自己写 SVG。' +
      '只输出一个 JSON 对象，不要 Markdown：' +
      '{"drawing":{"durationMs":6000,"items":[' +
      '{"emoji":"🐈","x":98,"y":126,"size":150,"rotation":0,"flipX":false},' +
      '{"emoji":"🧶","x":190,"y":158,"size":62,"rotation":0,"flipX":false}]}}。' +
      '每个 item 的 emoji 必须是一个现有的、标准 Unicode emoji，不能是文字、字母、旗帜或自创符号。' +
      '只用 1 到 3 个 item。第一个是主体：单主体 size 用 176 到 196；' +
      '有配角时主体 size 用 130 到 164，配角用 42 到 82。' +
      '主体放在画面中心附近，配角只用于用户明确提到的场景；不添加无关装饰。' +
      '坐标是图片中心点，范围 0..240，必须保证整张图留在 8..232 内。' +
      'rotation 只能为 -35..35 的整数，flipX 是布尔值；不确定时分别用 0 和 false。' +
      '如果用户要求的对象没有完全对应的 emoji，选最接近且一眼可辨的一个，不拼凑怪物。' +
      'durationMs 固定使用 6000。' },
    { role: 'user', content: userText },
  ];
}

export function parseDrawingProposal(content) {
  return parseCuratedDrawing(content);
}

function parseSpeechSegments(structured, fallbackText, fallbackEmotion, fallbackIntensity) {
  const segments = structured?.speechSegments;
  if (!Array.isArray(segments) || segments.length < 1 || segments.length > 3) {
    return { segments: [{ text: fallbackText, emotion: fallbackEmotion, intensity: fallbackIntensity }],
      source: 'fallback_missing' };
  }
  const parsed = [];
  for (const segment of segments) {
    if (!segment || typeof segment !== 'object' || Array.isArray(segment) ||
        typeof segment.text !== 'string' || !segment.text.trim() ||
        segment.text.length > 700 || !emotions.has(segment.emotion) ||
        !Number.isInteger(segment.intensity) || segment.intensity < 20 ||
        segment.intensity > 100) {
      return { segments: [{ text: fallbackText, emotion: fallbackEmotion, intensity: fallbackIntensity }],
        source: 'fallback_invalid' };
    }
    parsed.push({ text: segment.text.trim(), emotion: segment.emotion,
      intensity: segment.intensity });
  }
  if (parsed.map((segment) => segment.text).join('').replace(/\s+/gu, '') !==
      fallbackText.replace(/\s+/gu, '')) {
    return { segments: [{ text: fallbackText, emotion: fallbackEmotion, intensity: fallbackIntensity }],
      source: 'fallback_mismatch' };
  }
  return { segments: parsed, source: 'model' };
}

function parseActionProposal(structured) {
  const action = structured?.action;
  if (action === undefined || action === null) {
    return { status: 'none', type: 'none', reason: '' };
  }
  const keys = action && typeof action === 'object' && !Array.isArray(action)
    ? Object.keys(action) : [];
  const allowedKeys = new Set(['type', 'direction', 'speedPerMille',
    'durationMs', 'lightMode']);
  if (!action || typeof action !== 'object' || Array.isArray(action) ||
      keys.length < 3 || keys.length > 5 ||
      !keys.every((key) => allowedKeys.has(key)) || action.type !== 'drive' ||
      !driveDirections.has(action.direction) ||
      !Number.isInteger(action.durationMs) || action.durationMs < 2000 ||
      action.durationMs > 9000 ||
      ('speedPerMille' in action && (!Number.isInteger(action.speedPerMille) ||
        action.speedPerMille < 700 || action.speedPerMille > 1000)) ||
      ('lightMode' in action && !lightModes.has(action.lightMode))) {
    return { status: 'rejected', type: 'none', reason: 'action_schema_invalid' };
  }
  return { status: 'shadow', type: 'drive', direction: action.direction,
    speedPerMille: action.speedPerMille || 900, durationMs: action.durationMs,
    lightMode: action.lightMode || 'unchanged', reason: '' };
}

function parseDrawingRequest(structured) {
  const drawing = structured?.drawing;
  if (drawing === undefined || drawing === null) return null;
  if (!drawing || typeof drawing !== 'object' || Array.isArray(drawing) ||
      Object.keys(drawing).length !== 1 || typeof drawing.subject !== 'string') return null;
  const subject = drawing.subject.trim();
  if (!subject || subject.length > 24 || /[\u0000-\u001f\u007f]/u.test(subject) ||
      /(?:https?:\/\/|www\.|[{}<>])/iu.test(subject)) return null;
  return { subject };
}

// Explicit commands remain a deterministic fallback when the model omits an
// action. Autonomous model actions use the same lower-level safety envelope.
export function explicitDriveIntent(text) {
  if (typeof text !== 'string') return null;
  const normalized = text.trim().replace(/[\s，。！？!？,]+/g, '').replace(/\.$/, '');
  const match = /^(?:喵喵同学)?(?:请|帮我|麻烦你|让你)?(?:小车|坦克|你)?(向前|往前|前进|向后|往后|后退|向左|往左|左转|向右|往右|右转)(?:走|开|移动|转)?(?:(?:持续)?([1-9](?:\.\d)?|[一二两三四五六七八九])秒|一下|一步|一点)?(?:吧|好吗|好不好)?$/.exec(normalized);
  if (!match) return null;
  const chineseSeconds = { 一: 1, 二: 2, 两: 2, 三: 3, 四: 4, 五: 5,
    六: 6, 七: 7, 八: 8, 九: 9 };
  const seconds = match[2] ? (chineseSeconds[match[2]] ?? Number(match[2])) : null;
  const durationMs = seconds === null ? null : Math.round(seconds * 1000);
  if (durationMs !== null && (durationMs < 2000 || durationMs > 9000)) return null;
  const direction = ['向前', '往前', '前进'].includes(match[1]) ? 'forward'
    : ['向后', '往后', '后退'].includes(match[1]) ? 'backward'
      : ['向左', '往左', '左转'].includes(match[1]) ? 'left' : 'right';
  return { direction, durationMs };
}

function trait(value, fallback) {
  const number = Number(value);
  return Number.isInteger(number) && number >= 0 && number <= 100 ? number : fallback;
}

export function conversationMessages(userText, history, personality, mood = null) {
  const curiosity = trait(personality.curiosity, 78);
  const warmth = trait(personality.warmth, 84);
  const talkativeness = trait(personality.talkativeness, 46);
  const traits = `当前人格参数（0-100）：好奇心=${curiosity}，亲近感=${warmth}，话语量=${talkativeness}。` +
    '好奇心影响探索和追问倾向，亲近感影响温柔程度，话语量影响回答长短；始终遵守前述安全要求。';
  const messages = [
    { role: 'system', content: systemPrompt + drawingPrompt },
    { role: 'system', content: traits },
    { role: 'system', content: moodPrompt(mood || {}) },
    ...history.flatMap(({ user, assistant }) => [
      { role: 'user', content: user }, { role: 'assistant', content: assistant },
    ]),
    { role: 'user', content: userText },
  ];
  if (explicitExploreIntent(userText)) {
    messages.splice(2, 0, { role: 'system', content:
      '本轮是拍照探索授权。先用不超过 18 个汉字的一句话回应，不要提前宣称看到了什么；' +
      '不输出 drive 动作。照片会在回答播放后拍摄，由另一轮视觉规划决定是否和如何行走。' });
  }
  if (explicitSleepIntent(userText)) {
    messages.splice(2, 0, { role: 'system', content:
      '用户明确要求结束本轮持续对话。简短道别，不提出新问题，不输出任何行走动作。' });
  }
  return messages;
}

export function parseConversationReply(content) {
  if (typeof content !== 'string' || !content.trim()) return null;
  let structured;
  try { structured = JSON.parse(content); } catch {
    const start = content.indexOf('{');
    const end = content.lastIndexOf('}');
    if (start >= 0 && end > start) {
      try { structured = JSON.parse(content.slice(start, end + 1)); } catch {}
    }
  }
  const reply = typeof structured?.reply === 'string' && structured.reply.trim()
    ? structured.reply.trim() : content.trim();
  if (!reply || reply.length > 2000) return null;
  const emotion = emotions.has(structured?.emotion) ? structured.emotion : 'calm';
  const intensity = Number.isFinite(structured?.intensity)
    ? Math.max(20, Math.min(100, Math.round(structured.intensity))) : 55;
  const { segments: speechSegments, source: speechSegmentSource } =
    parseSpeechSegments(structured, reply, emotion, intensity);
  return { text: reply, emotion: speechSegments[0].emotion,
    intensity: speechSegments[0].intensity, speechSegments, speechSegmentSource,
    thought: parseThought(structured?.thought, speechSegments.at(-1).emotion),
    cameraRequested: structured?.camera === 'look',
    drawing: parseDrawingRequest(structured),
    action: parseActionProposal(structured) };
}
