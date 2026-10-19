import sharp from 'sharp';
import { icons as fluentEmojiIcons, chars as fluentEmojiChars } from '@iconify-json/fluent-emoji-flat';

export const DRAWING_SIZE = 240;
export const DRAWING_BACKGROUND = '#071018';

const palette = new Set([
  'none', '#071018', '#59f3e8', '#fff1c7', '#ff8fb8', '#ffd166', '#8ee88b',
]);
const allowedElements = new Set([
  'svg', 'g', 'path', 'circle', 'ellipse', 'rect', 'line', 'polyline', 'polygon',
]);
const commonAttributes = new Set([
  'fill', 'stroke', 'stroke-width', 'stroke-linecap', 'stroke-linejoin', 'opacity',
]);
const elementAttributes = {
  svg: new Set(['viewBox', 'xmlns']),
  g: new Set(),
  path: new Set(['d']),
  circle: new Set(['cx', 'cy', 'r']),
  ellipse: new Set(['cx', 'cy', 'rx', 'ry']),
  rect: new Set(['x', 'y', 'width', 'height', 'rx', 'ry']),
  line: new Set(['x1', 'y1', 'x2', 'y2']),
  polyline: new Set(['points']),
  polygon: new Set(['points']),
};

function fail(code) {
  throw Object.assign(new Error(code), { code });
}

function parseJsonObject(content) {
  if (typeof content !== 'string') return null;
  const trimmed = content.trim();
  const unfenced = trimmed.match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i)?.[1] ?? trimmed;
  const start = unfenced.indexOf('{');
  const end = unfenced.lastIndexOf('}');
  if (start < 0 || end <= start) return null;
  try { return JSON.parse(unfenced.slice(start, end + 1)); } catch { return null; }
}

function iconNameForEmoji(emoji) {
  if (typeof emoji !== 'string' || emoji.length > 24) return null;
  const exact = [...emoji].map((character) => character.codePointAt(0).toString(16)).join('-');
  const withoutVariation = exact.split('-').filter((part) => part !== 'fe0f').join('-');
  const name = fluentEmojiChars[exact] || fluentEmojiChars[withoutVariation];
  return name && fluentEmojiIcons.icons[name] ? name : null;
}

/**
 * Parse a composition made only from bundled, designer-authored Fluent Emoji
 * SVG assets. The model chooses subject and layout, never vector markup.
 */
export function parseCuratedDrawing(content) {
  const parsed = parseJsonObject(content);
  const value = parsed?.drawing ?? parsed;
  if (!value || !Number.isInteger(value.durationMs) || value.durationMs < 4000 ||
      value.durationMs > 8000 || !Array.isArray(value.items) ||
      value.items.length < 1 || value.items.length > 3) return null;
  const items = [];
  for (const item of value.items) {
    const iconName = iconNameForEmoji(item?.emoji);
    const rotation = item?.rotation ?? 0;
    const flipX = item?.flipX ?? false;
    if (!iconName || !Number.isInteger(item.x) || !Number.isInteger(item.y) ||
        !Number.isInteger(item.size) || !Number.isInteger(rotation) ||
        typeof flipX !== 'boolean' || item.size < 42 || item.size > 196 ||
        rotation < -35 || rotation > 35 || item.x - item.size / 2 < 8 ||
        item.x + item.size / 2 > 232 || item.y - item.size / 2 < 8 ||
        item.y + item.size / 2 > 232) return null;
    items.push({ iconName, emoji: item.emoji, x: item.x, y: item.y,
      size: item.size, rotation, flipX });
  }
  if (items.length === 1 && items[0].size < 150) return null;
  return { durationMs: value.durationMs, items };
}

function finiteNumber(value, minimum = 0, maximum = DRAWING_SIZE) {
  if (!/^-?(?:\d+\.?\d*|\.\d+)$/.test(value)) return false;
  const number = Number(value);
  return Number.isFinite(number) && number >= minimum && number <= maximum;
}

function validatePaint(value) {
  return palette.has(value.toLowerCase());
}

function validateAttribute(name, value, tag) {
  if (name === 'viewBox') return tag === 'svg' && value === '0 0 240 240';
  if (name === 'xmlns') return tag === 'svg' && value === 'http://www.w3.org/2000/svg';
  if (name === 'fill' || name === 'stroke') return validatePaint(value);
  if (name === 'stroke-width') return finiteNumber(value, 2, 12);
  if (name === 'stroke-linecap' || name === 'stroke-linejoin') {
    return ['round', 'square', 'butt', 'miter', 'bevel'].includes(value);
  }
  // Very soft ground shadows and highlights are legitimate illustration
  // details. Opacity does not expand SVG capabilities, so accepting it down
  // to 0.05 is safe while avoiding false rejects of otherwise valid art.
  if (name === 'opacity') return finiteNumber(value, 0.05, 1);
  if (name === 'd') {
    return value.length <= 4000 && /^[MmLlHhVvCcSsQqTtAaZz0-9eE.,+\-\s]+$/.test(value) &&
      /[Mm]/.test(value);
  }
  if (name === 'points') {
    return value.length <= 2000 && /^[0-9.,+\-\s]+$/.test(value) &&
      (value.match(/-?(?:\d+\.?\d*|\.\d+)/g)?.length || 0) >= 4;
  }
  return finiteNumber(value);
}

/**
 * Accept only a small, self-contained SVG language. It deliberately excludes
 * text, CSS, links, images, filters, scripts and external resources before the
 * document reaches the rasterizer.
 */
export function sanitizeDrawingSvg(source) {
  if (typeof source !== 'string' || source.length < 80 || source.length > 12_000 ||
      /[&]|<!--|<!|<\?|\b(?:href|style|class|id|transform|filter|mask|clip-path)\s*=/i.test(source) ||
      /\b(?:url|data|javascript):/i.test(source)) fail('drawing_svg_unsafe');

  const svg = source.trim();
  if (!/^<svg\b/.test(svg) || !/<\/svg>$/.test(svg)) fail('drawing_svg_root_invalid');
  const tagPattern = /<\/?([A-Za-z][A-Za-z0-9-]*)([^<>]*)>/g;
  let cursor = 0;
  let elementCount = 0;
  let rootCount = 0;
  let match;
  while ((match = tagPattern.exec(svg)) !== null) {
    if (svg.slice(cursor, match.index).trim()) fail('drawing_svg_text_forbidden');
    cursor = tagPattern.lastIndex;
    const tag = match[1];
    const closing = match[0].startsWith('</');
    if (!allowedElements.has(tag)) fail('drawing_svg_element_forbidden');
    if (closing) {
      if (match[2].trim()) fail('drawing_svg_close_invalid');
      continue;
    }
    elementCount += tag === 'svg' ? 0 : 1;
    if (tag === 'svg') rootCount += 1;
    const seen = new Set();
    const attributes = match[2].replace(/\/$/, '');
    const attributePattern = /([A-Za-z][A-Za-z0-9-]*)\s*=\s*(?:"([^"]*)"|'([^']*)')/g;
    let attributeCursor = 0;
    let attribute;
    while ((attribute = attributePattern.exec(attributes)) !== null) {
      if (attributes.slice(attributeCursor, attribute.index).trim()) fail('drawing_svg_attribute_invalid');
      attributeCursor = attributePattern.lastIndex;
      const name = attribute[1];
      const value = attribute[2] ?? attribute[3];
      if (seen.has(name) ||
          (!commonAttributes.has(name) && !elementAttributes[tag].has(name)) ||
          !validateAttribute(name, value, tag)) fail('drawing_svg_attribute_invalid');
      seen.add(name);
    }
    if (attributes.slice(attributeCursor).trim()) fail('drawing_svg_attribute_invalid');
    if (tag === 'svg' && (!seen.has('viewBox') || rootCount !== 1)) fail('drawing_svg_viewbox_invalid');
    if (!['svg', 'g'].includes(tag) && !seen.has('fill') && !seen.has('stroke')) {
      fail('drawing_svg_paint_missing');
    }
  }
  if (svg.slice(cursor).trim() || rootCount !== 1 || elementCount < 3 || elementCount > 48) {
    fail('drawing_svg_structure_invalid');
  }
  return svg;
}

export function parseSvgDrawing(content) {
  try {
    const parsed = parseJsonObject(content);
    const value = parsed?.drawing ?? parsed;
    if (!value || !Number.isInteger(value.durationMs) || value.durationMs < 4000 ||
        value.durationMs > 8000) return null;
    return { durationMs: value.durationMs, svg: sanitizeDrawingSvg(value.svg) };
  } catch {
    return null;
  }
}

export async function rasterizeSvgDrawing(drawing) {
  const svg = sanitizeDrawingSvg(drawing?.svg);
  const image = await sharp(Buffer.from(svg), { density: 96, limitInputPixels: 1_000_000 })
    .resize(DRAWING_SIZE, DRAWING_SIZE, { fit: 'contain', background: DRAWING_BACKGROUND })
    .flatten({ background: DRAWING_BACKGROUND })
    .jpeg({ quality: 94, chromaSubsampling: '4:4:4', mozjpeg: true })
    .toBuffer();
  if (image.length < 256 || image.length > 96 * 1024) fail('drawing_raster_size_invalid');
  return { durationMs: drawing.durationMs, jpeg: image };
}

async function renderFluentEmoji(item) {
  const icon = fluentEmojiIcons.icons[item.iconName];
  const width = icon.width || fluentEmojiIcons.width || 32;
  const height = icon.height || fluentEmojiIcons.height || 32;
  const source = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}">${icon.body}</svg>`;
  const sized = await sharp(Buffer.from(source), { density: 192, limitInputPixels: 1_000_000 })
    .resize(item.size, item.size, { fit: 'contain' }).png().toBuffer();
  let pipeline = sharp(sized);
  if (item.flipX) pipeline = pipeline.flop();
  if (item.rotation) pipeline = pipeline.rotate(item.rotation,
    { background: { r: 0, g: 0, b: 0, alpha: 0 } });
  const transformed = await pipeline.png().toBuffer();
  if (!item.rotation) return transformed;
  // Rotation enlarges the pixel bounding box. Fit it back into the validated
  // size so the model cannot accidentally crop a corner at the screen edge.
  return sharp(transformed).resize(item.size, item.size,
    { fit: 'inside', withoutEnlargement: true }).png().toBuffer();
}

export async function rasterizeCuratedDrawing(drawing) {
  if (!drawing || !Array.isArray(drawing.items) || !drawing.items.length) {
    fail('drawing_curated_invalid');
  }
  const rendered = await Promise.all(drawing.items.map(renderFluentEmoji));
  const layers = [];
  for (let index = 0; index < rendered.length; index += 1) {
    const metadata = await sharp(rendered[index]).metadata();
    layers.push({ input: rendered[index],
      left: Math.round(drawing.items[index].x - metadata.width / 2),
      top: Math.round(drawing.items[index].y - metadata.height / 2) });
  }
  const image = await sharp({ create: { width: DRAWING_SIZE, height: DRAWING_SIZE,
    channels: 3, background: DRAWING_BACKGROUND } })
    .composite(layers)
    .jpeg({ quality: 94, chromaSubsampling: '4:4:4', mozjpeg: true })
    .toBuffer();
  if (image.length < 256 || image.length > 96 * 1024) fail('drawing_raster_size_invalid');
  return { durationMs: drawing.durationMs, jpeg: image };
}

export async function rasterizeDrawing(drawing) {
  return Array.isArray(drawing?.items)
    ? rasterizeCuratedDrawing(drawing) : rasterizeSvgDrawing(drawing);
}
