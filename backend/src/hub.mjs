// Core pager logic: call/voicemail events, canned replies, sync. No network code in here -
// Twilio and MQTT are injected, which is what makes this testable.
import { DatabaseSync } from 'node:sqlite';
import { T, REPLY_STATUS, VM_ACTION, MAX_NAME, MAX_NUMBER, MAX_STR, clip, chunkBytes, encode, decode }
  from './proto.mjs';

export const LIVE_REPLY_LOOKBACK_S = 30;   // "the call I am looking at" = reached us this recently
export const QUEUED_REPLY_TTL_S = 120;     // ...or will reach us within this long
export const REPLY_COOLDOWN_S = 60;        // at most one SMS per caller per minute
export const SYNC_BATCH = 8;
export const MAX_CANNED = 8;

export const DEFAULT_CONFIG = Object.freeze({
  ownerName: 'me',
  greeting: 'Hi. I am away from my phone. Leave a message after the tone and it will reach my pager.',
  tzMinutes: 0,
  canned: [
    'Away from my phone. I will call you back in 10 minutes.',
    'Away from my phone. I will call you back within the hour.',
    'Cannot talk right now. If it is urgent, please leave a voicemail and it will reach me.',
    'Got your call. I will get back to you today.',
  ],
});

const E164 = /^\+[1-9]\d{6,14}$/;

export class Hub {
  /**
   * @param {object} o
   * @param {string} [o.dbPath]
   * @param {(to: string, body: string) => Promise<void>} o.sendSms
   * @param {() => number} [o.now] unix seconds
   * @param {(frames: Uint8Array[]) => void} [o.push] deliver frames to the pager over LTE
   */
  constructor({ dbPath = ':memory:', sendSms, now = () => Math.floor(Date.now() / 1000), push = () => {} }) {
    this.db = new DatabaseSync(dbPath);
    this.sendSms = sendSms;
    this.now = now;
    this.push = push;
    this.seq = 0;
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS contacts (number TEXT PRIMARY KEY, name TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS events (
        id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, ts INTEGER NOT NULL,
        number TEXT NOT NULL, call_sid TEXT, vm_id INTEGER);
      CREATE TABLE IF NOT EXISTS voicemails (
        id INTEGER PRIMARY KEY AUTOINCREMENT, call_sid TEXT UNIQUE, ts INTEGER NOT NULL,
        number TEXT NOT NULL, duration INTEGER NOT NULL DEFAULT 0, recording_url TEXT,
        transcript TEXT NOT NULL DEFAULT '', heard INTEGER NOT NULL DEFAULT 0,
        deleted INTEGER NOT NULL DEFAULT 0);
      CREATE TABLE IF NOT EXISTS replies (
        id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, number TEXT NOT NULL,
        body TEXT NOT NULL, ok INTEGER NOT NULL);
    `);
  }

  // ------------------------------------------------------------------ config
  getConfig() {
    const row = this.db.prepare('SELECT v FROM kv WHERE k = ?').get('config');
    const saved = row ? JSON.parse(row.v) : {};
    return { ...DEFAULT_CONFIG, ...saved, cfgVer: saved.cfgVer ?? 1 };
  }

  setConfig(patch) {
    const cur = this.getConfig();
    const next = { ...cur };
    if (patch.ownerName !== undefined) next.ownerName = clip(String(patch.ownerName).trim(), 40) || 'me';
    if (patch.greeting !== undefined) next.greeting = String(patch.greeting).trim().slice(0, 500);
    if (patch.tzMinutes !== undefined) {
      const tz = Number(patch.tzMinutes);
      if (!Number.isInteger(tz) || Math.abs(tz) > 14 * 60) throw new Error('tzMinutes out of range');
      next.tzMinutes = tz;
    }
    if (patch.canned !== undefined) {
      if (!Array.isArray(patch.canned) || patch.canned.length < 1 || patch.canned.length > MAX_CANNED) {
        throw new Error(`canned must be a list of 1 to ${MAX_CANNED} messages`);
      }
      next.canned = patch.canned.map((t) => clip(String(t).trim(), MAX_STR));
      if (next.canned.some((t) => !t)) throw new Error('canned messages cannot be empty');
    }
    next.cfgVer = (cur.cfgVer % 0xffff) + 1;
    this.db.prepare('INSERT INTO kv (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v')
      .run('config', JSON.stringify(next));
    return next;
  }

  setContacts(list) {
    this.db.exec('BEGIN');
    try {
      this.db.exec('DELETE FROM contacts');
      const ins = this.db.prepare('INSERT OR REPLACE INTO contacts (number, name) VALUES (?, ?)');
      for (const c of list) {
        if (E164.test(c.number)) ins.run(c.number, clip(String(c.name), MAX_NAME));
      }
      this.db.exec('COMMIT');
    } catch (e) {
      this.db.exec('ROLLBACK');
      throw e;
    }
  }

  nameFor(number) {
    return this.db.prepare('SELECT name FROM contacts WHERE number = ?').get(number)?.name ?? '';
  }

  // ------------------------------------------------------------------ telephony side
  /** A call was forwarded to the voicemail number (declined, unanswered or phone unreachable). */
  async onIncomingCall({ callSid, from }) {
    const number = E164.test(from ?? '') ? from : '';
    const existing = this.db.prepare("SELECT id FROM events WHERE call_sid = ? AND kind = 'call'").get(callSid);
    if (existing) return existing.id;
    const ts = this.now();
    const id = Number(this.db.prepare(
      "INSERT INTO events (kind, ts, number, call_sid) VALUES ('call', ?, ?, ?)").run(ts, number, callSid)
      .lastInsertRowid);
    const queued = this.#takeQueuedReply();
    this.push([this.#frameFor(this.#event(id))]);
    if (queued !== null) {
      const status = await this.#sendReply(number, queued);
      this.push([encode({ type: 'REPLY_RESULT', seq: this.#nextSeq(), call: id, status })]);
    }
    return id;
  }

  /** The caller finished recording. The transcript usually follows a little later. */
  onRecording({ callSid, recordingUrl, duration }) {
    const call = this.db.prepare("SELECT ts, number FROM events WHERE call_sid = ? AND kind = 'call'").get(callSid);
    const dur = Math.max(0, Math.min(0xffff, Number(duration) || 0));
    if (dur < 2) return null;   // hang-ups: the call event alone is enough
    this.db.prepare(`INSERT INTO voicemails (call_sid, ts, number, duration, recording_url)
      VALUES (?, ?, ?, ?, ?)
      ON CONFLICT(call_sid) DO UPDATE SET duration = excluded.duration, recording_url = excluded.recording_url`)
      .run(callSid, call?.ts ?? this.now(), call?.number ?? '', dur, recordingUrl ?? null);
    const vm = this.db.prepare('SELECT id FROM voicemails WHERE call_sid = ?').get(callSid);
    return this.#vmEvent(vm.id);
  }

  onTranscription({ callSid, text }) {
    const vm = this.db.prepare('SELECT id FROM voicemails WHERE call_sid = ?').get(callSid);
    if (!vm) return null;
    const clean = String(text ?? '').replace(/\s+/g, ' ').trim().slice(0, 4000);
    this.db.prepare('UPDATE voicemails SET transcript = ? WHERE id = ?').run(clean, vm.id);
    return this.#vmEvent(vm.id);
  }

  #vmEvent(vmId) {
    const vm = this.db.prepare('SELECT * FROM voicemails WHERE id = ?').get(vmId);
    const id = Number(this.db.prepare(
      "INSERT INTO events (kind, ts, number, call_sid, vm_id) VALUES ('vm', ?, ?, ?, ?)")
      .run(vm.ts, vm.number, vm.call_sid, vm.id).lastInsertRowid);
    this.push([this.#frameFor(this.#event(id))]);
    return id;
  }

  // ------------------------------------------------------------------ app side
  listVoicemails() {
    return this.db.prepare(`SELECT id, ts, number, duration, transcript, heard FROM voicemails
      WHERE deleted = 0 ORDER BY id DESC LIMIT 100`).all()
      .map((v) => ({ ...v, heard: !!v.heard, name: this.nameFor(v.number) }));
  }

  recordingUrl(vmId) {
    return this.db.prepare('SELECT recording_url FROM voicemails WHERE id = ? AND deleted = 0')
      .get(vmId)?.recording_url ?? null;
  }

  markVoicemail(vmId, action) {
    const col = action === VM_ACTION.DELETE ? 'deleted' : action === VM_ACTION.HEARD ? 'heard' : null;
    if (!col) return false;
    return this.db.prepare(`UPDATE voicemails SET ${col} = 1 WHERE id = ?`).run(vmId).changes > 0;
  }

  // ------------------------------------------------------------------ pager side
  /** Handle one frame from the pager; returns the frames to send back. */
  async handleUp(bytes) {
    const m = decode(bytes);
    switch (m.type) {
      case 'HELLO': {
        const cfg = this.getConfig();
        const out = [encode({ type: 'TIME', seq: this.#nextSeq(), unix: this.now(), tz: cfg.tzMinutes })];
        if (m.cfg !== cfg.cfgVer) {
          cfg.canned.forEach((text, idx) => out.push(encode({
            type: 'CANNED', seq: this.#nextSeq(), cfg: cfg.cfgVer, idx, count: cfg.canned.length, text })));
        }
        return out.concat(this.#sync(m.last));
      }
      case 'SYNC':
        return this.#sync(m.last);
      case 'REPLY':
        return [encode({ type: 'REPLY_RESULT', seq: this.#nextSeq(), ...(await this.#reply(m)) })];
      case 'VM_GET': {
        const vm = this.db.prepare('SELECT transcript FROM voicemails WHERE id = ? AND deleted = 0').get(m.vm);
        const text = vm?.transcript ?? '';
        const total = Buffer.byteLength(text);
        const chunk = chunkBytes(text).find((c) => c.offset === m.offset) ?? { offset: m.offset, text: '' };
        return [encode({ type: 'VM_TEXT', seq: this.#nextSeq(), vm: m.vm, offset: chunk.offset,
          total: Math.min(total, 0xffff), chunk: chunk.text })];
      }
      case 'VM_MARK':
        this.markVoicemail(m.vm, m.action);
        return this.#sync(0xffffffff);
      default:
        throw new Error(`unexpected ${m.type} from pager`);
    }
  }

  #sync(last) {
    const rows = this.db.prepare('SELECT id FROM events WHERE id > ? ORDER BY id LIMIT ?').all(last, SYNC_BATCH);
    const out = rows.map((r) => this.#frameFor(this.#event(r.id)));
    const newest = this.db.prepare('SELECT MAX(id) AS id FROM events').get().id ?? 0;
    const reached = rows.length ? rows[rows.length - 1].id : Math.min(last, newest);
    const unheard = this.db.prepare('SELECT COUNT(*) AS n FROM voicemails WHERE heard = 0 AND deleted = 0').get().n;
    out.push(encode({ type: 'SYNC_DONE', seq: this.#nextSeq(), last: reached, unheard: Math.min(unheard, 255) }));
    return out;
  }

  async #reply({ call, idx }) {
    const cfg = this.getConfig();
    const text = cfg.canned[idx];
    if (!text) return { call, status: REPLY_STATUS.FAILED };
    if (call !== 0) {
      const ev = this.db.prepare("SELECT number FROM events WHERE id = ? AND kind IN ('call', 'vm')").get(call);
      if (!ev || !ev.number) return { call, status: REPLY_STATUS.NO_CALLER };
      return { call, status: await this.#sendReply(ev.number, text) };
    }
    // call == 0: "whoever is calling right now". Only ever match a call that reached us in the
    // last few seconds; otherwise hold the reply for the call that is about to be forwarded.
    // Never fall back to an older caller - that would text the wrong person.
    const live = this.db.prepare(
      "SELECT id, number FROM events WHERE kind = 'call' AND ts >= ? ORDER BY id DESC LIMIT 1")
      .get(this.now() - LIVE_REPLY_LOOKBACK_S);
    if (live) {
      if (!live.number) return { call: live.id, status: REPLY_STATUS.NO_CALLER };
      return { call: live.id, status: await this.#sendReply(live.number, text) };
    }
    this.db.prepare('INSERT INTO kv (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v')
      .run('queued_reply', JSON.stringify({ text, until: this.now() + QUEUED_REPLY_TTL_S }));
    return { call: 0, status: REPLY_STATUS.QUEUED };
  }

  #takeQueuedReply() {
    const row = this.db.prepare('SELECT v FROM kv WHERE k = ?').get('queued_reply');
    if (!row) return null;
    this.db.prepare('DELETE FROM kv WHERE k = ?').run('queued_reply');
    const q = JSON.parse(row.v);
    return q.until >= this.now() ? q.text : null;
  }

  async #sendReply(number, text) {
    if (!E164.test(number)) return REPLY_STATUS.NO_CALLER;   // withheld / anonymous caller
    const recent = this.db.prepare('SELECT 1 FROM replies WHERE number = ? AND ok = 1 AND ts > ?')
      .get(number, this.now() - REPLY_COOLDOWN_S);
    if (recent) return REPLY_STATUS.FAILED;
    const body = `${this.getConfig().ownerName} (auto-reply from my pager): ${text}`;
    let ok = 1;
    try {
      await this.sendSms(number, body);
    } catch {
      ok = 0;
    }
    this.db.prepare('INSERT INTO replies (ts, number, body, ok) VALUES (?, ?, ?, ?)').run(this.now(), number, body, ok);
    return ok ? REPLY_STATUS.SENT : REPLY_STATUS.FAILED;
  }

  #event(id) {
    return this.db.prepare(`SELECT e.*, v.duration, v.transcript, v.deleted FROM events e
      LEFT JOIN voicemails v ON v.id = e.vm_id WHERE e.id = ?`).get(id);
  }

  #frameFor(ev) {
    const number = clip(ev.number, MAX_NUMBER);
    const name = this.nameFor(ev.number);
    if (ev.kind === 'vm') {
      return encode({ type: 'EV_VM', seq: this.#nextSeq(), event: ev.id, vm: ev.vm_id, ts: ev.ts,
        dur: ev.duration, len: Math.min(Buffer.byteLength(ev.transcript ?? ''), 0xffff), number, name });
    }
    return encode({ type: 'EV_CALL', seq: this.#nextSeq(), event: ev.id, ts: ev.ts, flags: 0, number, name });
  }

  #nextSeq() {
    this.seq = (this.seq + 1) & 0xff;
    return this.seq;
  }
}

export { T };
