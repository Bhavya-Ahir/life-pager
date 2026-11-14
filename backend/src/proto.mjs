// Pager wire protocol v1 (see docs/protocol.md). Mirrors firmware/src/proto.c byte for byte;
// both are checked against protocol/vectors.txt.

export const PROTO_VERSION = 1;
export const MAX_FRAME = 180;
export const MAX_STR = 160;
export const MAX_NUMBER = 20;
export const MAX_NAME = 32;

export const T = Object.freeze({
  HELLO: 0x01, SYNC: 0x02, REPLY: 0x03, VM_GET: 0x04, VM_MARK: 0x05,
  TIME: 0x81, CANNED: 0x82, EV_CALL: 0x83, EV_VM: 0x84, VM_TEXT: 0x85,
  REPLY_RESULT: 0x86, SYNC_DONE: 0x87,
});
const NAME = Object.fromEntries(Object.entries(T).map(([k, v]) => [v, k]));

export const REPLY_STATUS = Object.freeze({ SENT: 0, QUEUED: 1, NO_CALLER: 2, FAILED: 3 });
export const VM_ACTION = Object.freeze({ HEARD: 1, DELETE: 2 });

// field kinds: u8 u16 i16 u32, s:<max> = length-prefixed UTF-8
const LAYOUT = {
  HELLO: [['proto', 'u8'], ['fwMajor', 'u8'], ['fwMinor', 'u8'], ['batt', 'u8'], ['link', 'u8'],
    ['cfg', 'u16'], ['last', 'u32']],
  SYNC: [['last', 'u32']],
  REPLY: [['call', 'u32'], ['idx', 'u8']],
  VM_GET: [['vm', 'u32'], ['offset', 'u16']],
  VM_MARK: [['vm', 'u32'], ['action', 'u8']],
  TIME: [['unix', 'u32'], ['tz', 'i16']],
  CANNED: [['cfg', 'u16'], ['idx', 'u8'], ['count', 'u8'], ['text', `s:${MAX_STR}`]],
  EV_CALL: [['event', 'u32'], ['ts', 'u32'], ['flags', 'u8'], ['number', `s:${MAX_NUMBER}`],
    ['name', `s:${MAX_NAME}`]],
  EV_VM: [['event', 'u32'], ['vm', 'u32'], ['ts', 'u32'], ['dur', 'u16'], ['len', 'u16'],
    ['number', `s:${MAX_NUMBER}`], ['name', `s:${MAX_NAME}`]],
  VM_TEXT: [['vm', 'u32'], ['offset', 'u16'], ['total', 'u16'], ['chunk', `s:${MAX_STR}`]],
  REPLY_RESULT: [['call', 'u32'], ['status', 'u8']],
  SYNC_DONE: [['last', 'u32'], ['unheard', 'u8']],
};

const enc = new TextEncoder();
const dec = new TextDecoder('utf-8', { fatal: true });

/** Cut a string so its UTF-8 form fits `max` bytes without splitting a code point. */
export function clip(str, max) {
  let out = '';
  let n = 0;
  for (const ch of String(str ?? '')) {
    const l = enc.encode(ch).length;
    if (n + l > max) break;
    out += ch;
    n += l;
  }
  return out;
}

/** Split text into chunks of at most `max` UTF-8 bytes. Returns [{offset, bytes}]. */
export function chunkBytes(str, max = MAX_STR) {
  const chunks = [];
  let offset = 0;
  let cur = '';
  let n = 0;
  for (const ch of String(str ?? '')) {
    const l = enc.encode(ch).length;
    if (n + l > max) {
      chunks.push({ offset, text: cur });
      offset += n;
      cur = '';
      n = 0;
    }
    cur += ch;
    n += l;
  }
  if (cur || chunks.length === 0) chunks.push({ offset, text: cur });
  return chunks;
}

export function encode(msg) {
  const layout = LAYOUT[msg.type];
  if (!layout) throw new Error(`unknown message type ${msg.type}`);
  const out = [T[msg.type], (msg.seq ?? 0) & 0xff];
  for (const [field, kind] of layout) {
    const v = msg[field];
    if (kind.startsWith('s:')) {
      const bytes = enc.encode(String(v ?? ''));
      if (bytes.length > Number(kind.slice(2))) throw new Error(`${msg.type}.${field} too long`);
      out.push(bytes.length, ...bytes);
      continue;
    }
    if (!Number.isInteger(v)) throw new Error(`${msg.type}.${field} must be an integer`);
    const n = kind === 'i16' ? v & 0xffff : v;
    const size = kind === 'u8' ? 1 : kind === 'u32' ? 4 : 2;
    if (n < 0 || n >= 2 ** (8 * size)) throw new Error(`${msg.type}.${field} out of range`);
    for (let i = 0; i < size; i++) out.push(Math.floor(n / 2 ** (8 * i)) & 0xff);
  }
  if (out.length > MAX_FRAME) throw new Error(`${msg.type} frame too long`);
  return Uint8Array.from(out);
}

export function decode(buf) {
  const b = buf instanceof Uint8Array ? buf : Uint8Array.from(buf);
  if (b.length < 2 || b.length > MAX_FRAME) throw new Error('bad frame length');
  const type = NAME[b[0]];
  if (!type) throw new Error(`unknown frame type 0x${b[0].toString(16)}`);
  const msg = { type, seq: b[1] };
  let n = 2;
  const need = (k) => { if (n + k > b.length) throw new Error('truncated frame'); };
  for (const [field, kind] of LAYOUT[type]) {
    if (kind.startsWith('s:')) {
      need(1);
      const l = b[n++];
      if (l > Number(kind.slice(2))) throw new Error(`${type}.${field} too long`);
      need(l);
      msg[field] = dec.decode(b.subarray(n, n + l));
      if (msg[field].includes('\0')) throw new Error('embedded NUL');
      n += l;
      continue;
    }
    const size = kind === 'u8' ? 1 : kind === 'u32' ? 4 : 2;
    need(size);
    let v = 0;
    for (let i = 0; i < size; i++) v += b[n + i] * 2 ** (8 * i);
    if (kind === 'i16' && v >= 0x8000) v -= 0x10000;
    msg[field] = v;
    n += size;
  }
  if (n !== b.length) throw new Error('trailing bytes');
  return msg;
}

/** Canonical one-line text form; identical to proto_dump() in the firmware. */
export function dump(m) {
  const s = `seq=${m.seq}`;
  switch (m.type) {
    case 'HELLO': return `HELLO ${s} proto=${m.proto} fw=${m.fwMajor}.${m.fwMinor} batt=${m.batt} link=${m.link} cfg=${m.cfg} last=${m.last}`;
    case 'SYNC': return `SYNC ${s} last=${m.last}`;
    case 'REPLY': return `REPLY ${s} call=${m.call} idx=${m.idx}`;
    case 'VM_GET': return `VM_GET ${s} vm=${m.vm} offset=${m.offset}`;
    case 'VM_MARK': return `VM_MARK ${s} vm=${m.vm} action=${m.action}`;
    case 'TIME': return `TIME ${s} unix=${m.unix} tz=${m.tz}`;
    case 'CANNED': return `CANNED ${s} cfg=${m.cfg} idx=${m.idx} count=${m.count} text=[${m.text}]`;
    case 'EV_CALL': return `EV_CALL ${s} event=${m.event} ts=${m.ts} flags=${m.flags} number=[${m.number}] name=[${m.name}]`;
    case 'EV_VM': return `EV_VM ${s} event=${m.event} vm=${m.vm} ts=${m.ts} dur=${m.dur} len=${m.len} number=[${m.number}] name=[${m.name}]`;
    case 'VM_TEXT': return `VM_TEXT ${s} vm=${m.vm} offset=${m.offset} total=${m.total} chunk=[${m.chunk}]`;
    case 'REPLY_RESULT': return `REPLY_RESULT ${s} call=${m.call} status=${m.status}`;
    case 'SYNC_DONE': return `SYNC_DONE ${s} last=${m.last} unheard=${m.unheard}`;
    default: throw new Error('unknown type');
  }
}

export const toHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
export const fromHex = (h) => Uint8Array.from(h.match(/../g) ?? [], (x) => parseInt(x, 16));
