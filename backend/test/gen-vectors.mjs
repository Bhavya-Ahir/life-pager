// Regenerates protocol/vectors.txt. The file is the contract between firmware and backend:
// each line is "<frame hex> | <canonical dump>".
import { writeFileSync } from 'node:fs';
import { encode, dump, toHex } from '../src/proto.mjs';

export const SAMPLES = [
  { type: 'HELLO', seq: 1, proto: 1, fwMajor: 0, fwMinor: 1, batt: 87, link: 1, cfg: 3, last: 42 },
  { type: 'HELLO', seq: 255, proto: 1, fwMajor: 2, fwMinor: 9, batt: 100, link: 0, cfg: 65535, last: 4294967295 },
  { type: 'SYNC', seq: 2, last: 0 },
  { type: 'REPLY', seq: 3, call: 0, idx: 2 },
  { type: 'REPLY', seq: 4, call: 70000, idx: 7 },
  { type: 'VM_GET', seq: 5, vm: 12, offset: 320 },
  { type: 'VM_MARK', seq: 6, vm: 12, action: 2 },
  { type: 'TIME', seq: 7, unix: 1794700800, tz: -480 },
  { type: 'TIME', seq: 8, unix: 1794700800, tz: 330 },
  { type: 'CANNED', seq: 9, cfg: 3, idx: 0, count: 4, text: 'Away from my phone. I will call you back in 10 minutes.' },
  { type: 'CANNED', seq: 10, cfg: 3, idx: 1, count: 4, text: 'x'.repeat(160) },
  { type: 'EV_CALL', seq: 11, event: 43, ts: 1794700801, flags: 0, number: '+14155550123', name: 'Asha Patel' },
  { type: 'EV_CALL', seq: 12, event: 44, ts: 1794700900, flags: 1, number: '', name: '' },
  { type: 'EV_VM', seq: 13, event: 45, vm: 12, ts: 1794700801, dur: 37, len: 412, number: '+14155550123', name: 'Zoë' },
  { type: 'VM_TEXT', seq: 14, vm: 12, offset: 0, total: 26, chunk: 'Hey, call me back tonight.' },
  { type: 'VM_TEXT', seq: 15, vm: 12, offset: 160, total: 161, chunk: '' },
  { type: 'REPLY_RESULT', seq: 16, call: 43, status: 0 },
  { type: 'SYNC_DONE', seq: 17, last: 45, unheard: 1 },
];

if (import.meta.url === `file://${process.argv[1]}`) {
  const lines = SAMPLES.map((m) => `${toHex(encode(m))} | ${dump(m)}`);
  writeFileSync(new URL('../../protocol/vectors.txt', import.meta.url), lines.join('\n') + '\n');
  console.log(`wrote ${lines.length} vectors`);
}
