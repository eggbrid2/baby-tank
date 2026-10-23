import sharp from 'sharp';

export const DRAWING_SIZE = 240;
export const DRAWING_BACKGROUND = '#071018';
export const DRAWING_DISPLAY_MS = 15_000;

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
    const numbers = value.match(/-?(?:\d+\.?\d*|\.\d+)/g) || [];
    return value.length <= 4000 && numbers.length <= 256 &&
      /^[MmLlHhVvCcSsQqTtAaZz0-9.,+\-\s]+$/.test(value) &&
      numbers.every((number) => Math.abs(Number(number)) <= 500) &&
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
  const openTags = [];
  let match;
  while ((match = tagPattern.exec(svg)) !== null) {
    if (svg.slice(cursor, match.index).trim()) fail('drawing_svg_text_forbidden');
    cursor = tagPattern.lastIndex;
    const tag = match[1];
    const closing = match[0].startsWith('</');
    if (!allowedElements.has(tag)) fail('drawing_svg_element_forbidden');
    if (closing) {
      if (match[2].trim() || openTags.pop() !== tag) fail('drawing_svg_close_invalid');
      continue;
    }
    const selfClosing = /\/\s*>$/.test(match[0]);
    if (rootCount === 0 && tag !== 'svg') fail('drawing_svg_root_invalid');
    if (tag !== 'svg' && openTags.length === 0) fail('drawing_svg_root_invalid');
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
    if (!selfClosing) openTags.push(tag);
  }
  if (svg.slice(cursor).trim() || openTags.length !== 0 || rootCount !== 1 ||
      elementCount < 4 || elementCount > 48) {
    fail('drawing_svg_structure_invalid');
  }
  return svg;
}

export function parseSvgDrawing(content) {
  try {
    const parsed = parseJsonObject(content);
    const value = parsed?.drawing ?? parsed;
    if (!value || !Number.isInteger(value.durationMs) ||
        value.durationMs !== DRAWING_DISPLAY_MS ||
        typeof value.svg !== 'string' || Object.keys(value).some((key) =>
          key !== 'durationMs' && key !== 'svg')) return null;
    return { durationMs: value.durationMs, svg: sanitizeDrawingSvg(value.svg) };
  } catch {
    return null;
  }
}

export async function rasterizeSvgDrawing(drawing) {
  const svg = sanitizeDrawingSvg(drawing?.svg);
  const pixels = await sharp(Buffer.from(svg), { density: 96, limitInputPixels: 1_000_000 })
    .resize(DRAWING_SIZE, DRAWING_SIZE, { fit: 'contain', background: DRAWING_BACKGROUND })
    .flatten({ background: DRAWING_BACKGROUND })
    .removeAlpha().raw().toBuffer();
  // An empty canvas is rejected before it can occupy the child's screen.
  // This is only a visibility floor; recognizability still needs real review.
  let painted = 0;
  let left = DRAWING_SIZE;
  let top = DRAWING_SIZE;
  let right = -1;
  let bottom = -1;
  for (let offset = 0; offset < pixels.length; offset += 3) {
    if (Math.abs(pixels[offset] - 7) + Math.abs(pixels[offset + 1] - 16) +
        Math.abs(pixels[offset + 2] - 24) > 45) {
      painted += 1;
      const pixel = offset / 3;
      const x = pixel % DRAWING_SIZE;
      const y = Math.floor(pixel / DRAWING_SIZE);
      left = Math.min(left, x);
      right = Math.max(right, x);
      top = Math.min(top, y);
      bottom = Math.max(bottom, y);
    }
  }
  if (painted < 1000 || painted > DRAWING_SIZE * DRAWING_SIZE * 0.9 ||
      right - left < 70 || bottom - top < 70) {
    fail('drawing_subject_visibility_invalid');
  }
  const image = await sharp(pixels, { raw: { width: DRAWING_SIZE,
    height: DRAWING_SIZE, channels: 3 } })
    // ESP32's JPEG decoder needs sequential baseline scans. mozjpeg may
    // produce SOF2/progressive output even for a small 240px drawing.
    .jpeg({ quality: 94, chromaSubsampling: '4:4:4', progressive: false })
    .toBuffer();
  if (image.length < 256 || image.length > 96 * 1024) fail('drawing_raster_size_invalid');
  return { durationMs: DRAWING_DISPLAY_MS, jpeg: image };
}

export async function rasterizeDrawing(drawing) {
  return rasterizeSvgDrawing(drawing);
}
