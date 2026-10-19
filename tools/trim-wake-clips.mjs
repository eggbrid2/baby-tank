import { readFileSync, writeFileSync } from 'node:fs';

const names = process.argv.slice(2);
for (const name of names.length ? names : ['wozai', 'lailao', 'please-move', 'coming-through']) {
  const source = `firmware/head/assets/baby-tank-wake-${name}.pcm`;
  const destination = `firmware/head/assets/wake-${name}.pcm`;
  const pcm = readFileSync(source);
  const windowBytes = 320; // 10 ms of 16 kHz signed mono PCM.
  let first = 0;
  let last = 0;
  for (let offset = 0; offset + windowBytes <= pcm.length; offset += windowBytes) {
    let energy = 0;
    for (let index = offset; index < offset + windowBytes; index += 2) {
      energy += Math.abs(pcm.readInt16LE(index));
    }
    if (energy / (windowBytes / 2) > 300) {
      if (!first) first = offset;
      last = offset + windowBytes;
    }
  }
  if (!last) throw new Error(`No speech detected in ${source}`);
  const start = Math.max(0, first - 60 * 32) & ~1;
  const end = Math.min(pcm.length, last + 100 * 32) & ~1;
  writeFileSync(destination, pcm.subarray(start, end));
  process.stdout.write(`${name}: ${(end - start) / 32} ms\n`);
}
