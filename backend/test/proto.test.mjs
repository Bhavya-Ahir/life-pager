import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { encode, decode, dump, toHex, fromHex, chunkBytes, clip } from '../src/proto.mjs';

const lines = readFileSync(new URL('../../protocol/vectors.txt', import.meta.url), 'utf8').trim().split('\n');

test('every shared vector decodes to its dump and re-encodes to the same bytes', () => {
  assert.ok(lines.length >= 15);
  for (const line of lines) {
    const [hex, text] = line.split(' | ');
    const msg = decode(fromHex(hex));
    assert.equal(dump(msg), text);
    assert.equal(toHex(encode(msg)), hex);
  }
});

test('malformed frames are rejected', () => {
  const good = encode({ type: 'EV_CALL', seq: 1, event: 1, ts: 2, flags: 0, number: '+14155550123', name: 'A' });
  assert.throws(() => decode(good.subarray(0, good.length - 1)), /truncated/);
  assert.throws(() => decode(Uint8Array.from([...good, 0])), /trailing/);
  assert.throws(() => decode(Uint8Array.from([0x7f, 0])), /unknown/);
  assert.throws(() => decode(Uint8Array.from([0x01])), /length/);
  assert.throws(() => encode({ type: 'CANNED', seq: 0, cfg: 1, idx: 0, count: 1, text: 'x'.repeat(161) }), /too long/);
  assert.throws(() => encode({ type: 'SYNC', seq: 0, last: -1 }), /range/);
});

test('text is clipped and chunked on UTF-8 boundaries', () => {
  assert.equal(clip('héllo', 2), 'h');
  const text = 'é'.repeat(100);           // 200 bytes
  const chunks = chunkBytes(text, 160);
  assert.deepEqual(chunks.map((c) => c.offset), [0, 160]);
  assert.equal(chunks.map((c) => c.text).join(''), text);
  assert.deepEqual(chunkBytes(''), [{ offset: 0, text: '' }]);
});
