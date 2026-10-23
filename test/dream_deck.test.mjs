import test from 'node:test';
import assert from 'node:assert/strict';
import { DREAM_CATALOG, fallbackDreamDeck,
  parseDreamDeckReply } from '../src/dream_deck.mjs';

test('fallback dream deck contains all thirty safe local cards', () => {
  const deck = fallbackDreamDeck('tank-001:2026-10-22');
  assert.equal(deck.length, 30);
  assert.equal(new Set(deck.map((item) => `${item.key}:${item.displayText}`)).size, 30);
  assert.ok(deck.some((item) => item.key === 'fish'));
  assert.ok(deck.some((item) => item.key === 'bird'));
});

test('model can only reorder the complete allowlisted catalog', () => {
  const ids = DREAM_CATALOG.map((entry) => entry.id).reverse();
  const parsed = parseDreamDeckReply(JSON.stringify({ ids }), 'fallback');
  assert.equal(parsed.length, 30);
  assert.equal(parsed[0].key, DREAM_CATALOG.at(-1).key);

  const rejected = parseDreamDeckReply('{"ids":["fish-1"]}', 'fallback');
  assert.equal(rejected.length, 30);
  assert.notDeepEqual(rejected, parsed);
});
