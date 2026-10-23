// Deterministic character state. These are fictional play values, not health signals.
export const DEFAULT_NEEDS = Object.freeze({ hunger: 20, thirst: 20,
  energy: 70, stamina: 75, joy: 60, excitement: 35, bond: 55,
  curiosity: 60 });

export const EVENT_RULES = Object.freeze({
  fed: { hunger: -15, joy: 1 },
  drank: { thirst: -20 },
  played: { joy: 2, excitement: 3, stamina: -1 },
  side_touch: { bond: 2, excitement: 1 },
  poked: { bond: 2, excitement: 1 },
  rested: { energy: 24, stamina: 18, excitement: -8 },
  scene_completed: { joy: 1, curiosity: 1 },
  praised: { joy: 2, bond: 1 },
  shared_discovery: { curiosity: 2, joy: 1 },
  comforted: { joy: 1, bond: 1 },
});

const clamp = (value) => Math.max(0, Math.min(100, Math.round(value)));
const hourFormatters = new Map();

export function validTimeZone(value) {
  try {
    if (typeof value !== 'string' || value.length > 64) return false;
    new Intl.DateTimeFormat('en-US', { timeZone: value }).format(0);
    return true;
  } catch { return false; }
}

function localHour(at, timeZone) {
  let formatter = hourFormatters.get(timeZone);
  if (!formatter) {
    formatter = new Intl.DateTimeFormat('en-GB', { timeZone, hour: '2-digit',
      hourCycle: 'h23' });
    hourFormatters.set(timeZone, formatter);
  }
  const parts = formatter.formatToParts(at);
  return Number(parts.find((part) => part.type === 'hour')?.value);
}

function bandAt(at, timeZone) {
  const hour = localHour(new Date(at), timeZone);
  if (hour < 6 || hour >= 23) return 'late_night';
  if (hour >= 21) return 'evening';
  if (hour < 12) return 'morning';
  if (hour < 15) return 'midday';
  if (hour < 18) return 'afternoon';
  return 'early_evening';
}

export function timeBand(at, timeZone = 'Asia/Shanghai') {
  if (!validTimeZone(timeZone) || !Number.isFinite(new Date(at).getTime())) {
    throw new RangeError('invalid_clock_policy');
  }
  return bandAt(at, timeZone);
}

// Give the head a bounded UTC schedule instead of asking ESP-IDF to emulate
// the complete IANA timezone database. Transitions are minute-accurate,
// including DST changes, and the head fails closed after the horizon.
export function nightLockWindows(at, timeZone = 'Asia/Shanghai', days = 7) {
  if (!validTimeZone(timeZone) || !Number.isInteger(days) || days < 1 || days > 7) {
    throw new RangeError('invalid_clock_policy');
  }
  const start = new Date(at).getTime();
  if (!Number.isFinite(start)) throw new RangeError('invalid_clock_policy');
  const end = start + days * 86_400_000;
  const activeAt = (time) => bandAt(time, timeZone) === 'late_night';
  const windows = [];
  let open = activeAt(start) ? start : null;
  let previous = start;
  let active = open !== null;
  while (previous < end) {
    const next = Math.min(end, previous + 15 * 60_000);
    const nextActive = activeAt(next);
    if (nextActive !== active) {
      let low = previous;
      let high = next;
      while (high - low > 1000) {
        const middle = Math.floor((low + high) / 2);
        if (activeAt(middle) === active) low = middle;
        else high = middle;
      }
      const boundary = Math.floor(high / 60_000) * 60_000;
      if (active) windows.push([open, boundary]);
      else open = boundary;
      active = nextActive;
    }
    previous = next;
  }
  if (active) windows.push([open, end]);
  return { validUntilUtc: new Date(end).toISOString(),
    windowsUtc: windows.map(([from, to]) => [
      new Date(from).toISOString(), new Date(to).toISOString()]) };
}

export function projectNeeds(needs, updatedAt, at, timeZone = 'Asia/Shanghai') {
  const from = new Date(updatedAt).getTime();
  const until = new Date(at).getTime();
  if (!Number.isFinite(from) || !Number.isFinite(until) || !validTimeZone(timeZone)) {
    throw new RangeError('invalid_state_clock');
  }
  const next = Object.fromEntries(Object.entries(DEFAULT_NEEDS).map(([key, fallback]) =>
    [key, Number.isFinite(needs?.[key]) ? Math.max(0, Math.min(100, needs[key])) : fallback]));
  // Bound stale/offline catch-up to one day. Fifteen-minute UTC slices make
  // midnight and DST transitions deterministic without storing local times.
  let cursor = from;
  const end = Math.min(Math.max(until, from), from + 24 * 60 * 60_000);
  while (cursor < end) {
    const step = Math.min(end - cursor, 15 * 60_000);
    const hours = step / 3_600_000;
    const band = bandAt(cursor + Math.floor(step / 2), timeZone);
    const resting = band === 'evening' || band === 'late_night';
    next.hunger = Math.min(68, next.hunger + 1.5 * hours);
    next.thirst = Math.min(68, next.thirst + 2 * hours);
    next.energy = Math.max(20, Math.min(85, next.energy + (resting ? 2 : -1) * hours));
    next.stamina = Math.min(90, next.stamina + (resting ? 1.5 : 0.4) * hours);
    next.joy += (60 - next.joy) * Math.min(1, hours / 12);
    next.excitement += (35 - next.excitement) * Math.min(1, hours / 0.85);
    next.bond += (55 - next.bond) * Math.min(1, hours / 24);
    next.curiosity += (60 - next.curiosity) * Math.min(1, hours / 24);
    cursor += step;
  }
  return Object.fromEntries(Object.entries(next).map(([key, value]) => [key, clamp(value)]));
}

export function applyNeedEvent(needs, kind) {
  const delta = EVENT_RULES[kind];
  if (!delta) throw new RangeError('invalid_state_event');
  return Object.fromEntries(Object.entries(DEFAULT_NEEDS).map(([key, fallback]) =>
    [key, clamp((Number.isFinite(needs?.[key]) ? needs[key] : fallback) + (delta[key] || 0))]));
}

export function inferConversationNeedEvent(userText, replyText) {
  const user = String(userText || '').trim();
  const answer = String(replyText || '').trim();
  if (!user || answer.length < 4) return null;
  // Conservative, non-LLM classifier. A child's distress is not converted
  // into a demand to cheer up the toy. One bounded event at most per turn.
  if (/^(?:你真棒|你好厉害|谢谢你|做得好)[！!。.]?$/u.test(user)) return 'praised';
  if (/^(?:为什么|怎么|如何|什么是|告诉我)[^\n]{2,45}[？?]?$/u.test(user)) {
    return 'shared_discovery';
  }
  return null;
}

export function deriveCharacterState(needs, at, policy = {}) {
  const band = timeBand(at, policy.timeZone || 'Asia/Shanghai');
  const n = Object.fromEntries(Object.entries(DEFAULT_NEEDS).map(([key, fallback]) =>
    [key, clamp(Number.isFinite(needs?.[key]) ? needs[key] : fallback)]));
  const nightPenalty = band === 'late_night' ? 20 : band === 'evening' ? 10 : 0;
  const fatigue = clamp(100 - 0.65 * n.energy - 0.35 * n.stamina + nightPenalty);
  const spirit = clamp(0.6 * n.energy + 0.25 * n.joy + 0.15 * n.excitement - nightPenalty);
  const comfort = clamp(0.6 * n.joy + 0.25 * n.bond +
    0.15 * (100 - Math.max(n.hunger, n.thirst)));
  const activation = clamp(0.7 * n.excitement + 0.3 * n.energy - 0.2 * fatigue);
  const unmet = Math.max(n.hunger, n.thirst);
  const bothUnmet = Math.min(n.hunger, n.thirst);
  let mood = 'calm';
  // Needs win over generic positivity. This keeps a hungry, thirsty or tired
  // pet from looking permanently cheerful just because recent play raised joy.
  if (fatigue >= 62) mood = 'sleepy';
  else if (bothUnmet >= 58 && n.energy <= 45) mood = 'worried';
  else if (unmet >= 58) mood = activation >= 52 ? 'annoyed' : 'sad';
  else if (n.joy <= 34 || comfort <= 38) mood = 'sad';
  else if (n.excitement >= 78 && n.energy >= 58 && n.joy >= 65) mood = 'excited';
  else if (n.curiosity >= 68 && n.energy >= 35 && n.excitement < 72) mood = 'curious';
  else if (n.joy >= 72 && comfort >= 68 && unmet < 48) mood = 'happy';
  return { band, isNight: band === 'evening' || band === 'late_night',
    // A cloud policy flag is not a physical entry gate until head firmware
    // enforces it. Never claim that voice, BLE or motors are already locked.
    deepNightGateRequested: policy.deepNightLock === true && band === 'late_night',
    needs: n, derived: { fatigue, spirit, comfort, activation }, mood };
}
