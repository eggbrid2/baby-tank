import sharp from 'sharp';

export const SKETCH_MAX_BYTES = 96 * 1024;
export const SKETCH_GUESS_PROMPT =
  '你在和孩子玩猜画游戏。请识别白色画布上的手绘线条，只输出 JSON：' +
  '{"guess":"一个简短中文名词"}。guess 必须是 1 到 12 个字符，不要解释、' +
  '不要评价画得好坏、不要输出人名或敏感推断。不确定时猜最接近的普通物体；' +
  '完全无法辨认时返回 {"guess":"神秘图案"}。';

function jsonObject(content) {
  if (typeof content !== 'string') return null;
  const unfenced = content.trim().match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i)?.[1] ??
    content.trim();
  const start = unfenced.indexOf('{');
  const end = unfenced.lastIndexOf('}');
  if (start < 0 || end <= start) return null;
  try { return JSON.parse(unfenced.slice(start, end + 1)); } catch { return null; }
}

export function parseSketchGuess(content) {
  const parsed = jsonObject(content);
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed) ||
      Object.keys(parsed).some((key) => key !== 'guess') ||
      typeof parsed.guess !== 'string') return null;
  const guess = parsed.guess.trim();
  if (!guess || [...guess].length > 12 ||
      /[\u0000-\u001f\u007f<>\[\]{}]/u.test(guess)) return null;
  return guess;
}

/** Decode a bounded JPEG and flatten it to a predictable white 384px canvas.
 * A small ink floor rejects blank/tap-only submissions before they cost a
 * vision request. The normalized JPEG is ephemeral and is never persisted. */
export async function normalizeSketchJpeg(jpeg) {
  if (!Buffer.isBuffer(jpeg) || jpeg.length < 128 ||
      jpeg.length > SKETCH_MAX_BYTES || jpeg[0] !== 0xff || jpeg[1] !== 0xd8 ||
      jpeg.at(-2) !== 0xff || jpeg.at(-1) !== 0xd9) {
    throw Object.assign(new Error('sketch_jpeg_invalid'), { code: 'sketch_jpeg_invalid' });
  }
  let pixels;
  try {
    pixels = await sharp(jpeg, { limitInputPixels: 1_000_000 })
      .resize(384, 384, { fit: 'contain', background: '#fffdf7' })
      .flatten({ background: '#fffdf7' }).removeAlpha().raw().toBuffer();
  } catch {
    throw Object.assign(new Error('sketch_jpeg_invalid'), { code: 'sketch_jpeg_invalid' });
  }
  let ink = 0;
  for (let offset = 0; offset < pixels.length; offset += 3) {
    if (pixels[offset] + pixels[offset + 1] + pixels[offset + 2] < 690) ++ink;
  }
  if (ink < 180 || ink > 384 * 384 * 0.75) {
    throw Object.assign(new Error('sketch_ink_invalid'), { code: 'sketch_ink_invalid' });
  }
  return sharp(pixels, { raw: { width: 384, height: 384, channels: 3 } })
    .jpeg({ quality: 86, progressive: false, chromaSubsampling: '4:4:4' })
    .toBuffer();
}
