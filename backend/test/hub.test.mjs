import test from 'node:test';
import assert from 'node:assert/strict';
import { Hub, QUEUED_REPLY_TTL_S } from '../src/hub.mjs';
import { encode, decode, REPLY_STATUS } from '../src/proto.mjs';

function setup() {
  const sms = [];
  const pushed = [];
  const clock = { t: 1_794_700_000 };
  const hub = new Hub({
    sendSms: async (to, body) => { if (to === '+19990000000') throw new Error('boom'); sms.push({ to, body }); },
    now: () => clock.t,
    push: (frames) => pushed.push(...frames.map(decode)),
  });
  hub.setConfig({ ownerName: 'Bhavya' });
  const up = async (m) => (await hub.handleUp(encode({ seq: 1, ...m }))).map(decode);
  return { hub, sms, pushed, clock, up };
}
const ASHA = '+14155550123';

test('hello sends time, canned messages and pending events', async () => {
  const { hub, up } = setup();
  await hub.onIncomingCall({ callSid: 'CA1', from: ASHA });
  const out = await up({ type: 'HELLO', proto: 1, fwMajor: 0, fwMinor: 1, batt: 90, link: 0, cfg: 0, last: 0 });
  assert.deepEqual(out.map((m) => m.type), ['TIME', 'CANNED', 'CANNED', 'CANNED', 'CANNED', 'EV_CALL', 'SYNC_DONE']);
  assert.equal(out[5].number, ASHA);
  const cfg = hub.getConfig().cfgVer;
  const again = await up({ type: 'HELLO', proto: 1, fwMajor: 0, fwMinor: 1, batt: 90, link: 0, cfg, last: out[5].event });
  assert.deepEqual(again.map((m) => m.type), ['TIME', 'SYNC_DONE']);
});

test('reply to a specific call texts that caller with the owner prefix', async () => {
  const { hub, sms, up } = setup();
  const ev = await hub.onIncomingCall({ callSid: 'CA1', from: ASHA });
  const [r] = await up({ type: 'REPLY', call: ev, idx: 0 });
  assert.equal(r.status, REPLY_STATUS.SENT);
  assert.equal(sms.length, 1);
  assert.equal(sms[0].to, ASHA);
  assert.match(sms[0].body, /^Bhavya \(auto-reply from my pager\): Away from my phone/);
});

test('a live reply never goes to an older caller; it waits for the call being forwarded', async () => {
  const { hub, sms, pushed, clock, up } = setup();
  await hub.onIncomingCall({ callSid: 'CA-old', from: '+14155550999' });
  clock.t += 600;                                   // ten minutes later the phone rings again
  const [r] = await up({ type: 'REPLY', call: 0, idx: 1 });
  assert.equal(r.status, REPLY_STATUS.QUEUED);
  assert.equal(sms.length, 0);
  clock.t += 20;                                    // the declined call reaches voicemail
  await hub.onIncomingCall({ callSid: 'CA-new', from: ASHA });
  assert.deepEqual(sms.map((s) => s.to), [ASHA]);
  assert.equal(pushed.at(-1).type, 'REPLY_RESULT');
  assert.equal(pushed.at(-1).status, REPLY_STATUS.SENT);
});

test('a queued reply expires instead of hitting a later, unrelated caller', async () => {
  const { hub, sms, clock, up } = setup();
  await up({ type: 'REPLY', call: 0, idx: 0 });
  clock.t += QUEUED_REPLY_TTL_S + 1;
  await hub.onIncomingCall({ callSid: 'CA2', from: ASHA });
  assert.equal(sms.length, 0);
});

test('a live reply matches a call that arrived seconds ago', async () => {
  const { hub, sms, clock, up } = setup();
  await hub.onIncomingCall({ callSid: 'CA1', from: ASHA });
  clock.t += 10;
  const [r] = await up({ type: 'REPLY', call: 0, idx: 0 });
  assert.equal(r.status, REPLY_STATUS.SENT);
  assert.equal(sms[0].to, ASHA);
});

test('withheld numbers, bad indexes, cooldown and SMS failures are reported, not sent', async () => {
  const { hub, sms, clock, up } = setup();
  const anon = await hub.onIncomingCall({ callSid: 'CA1', from: 'anonymous' });
  assert.equal((await up({ type: 'REPLY', call: anon, idx: 0 }))[0].status, REPLY_STATUS.NO_CALLER);
  const ev = await hub.onIncomingCall({ callSid: 'CA2', from: ASHA });
  assert.equal((await up({ type: 'REPLY', call: ev, idx: 7 }))[0].status, REPLY_STATUS.FAILED);
  assert.equal((await up({ type: 'REPLY', call: ev, idx: 0 }))[0].status, REPLY_STATUS.SENT);
  assert.equal((await up({ type: 'REPLY', call: ev, idx: 1 }))[0].status, REPLY_STATUS.FAILED);   // cooldown
  clock.t += 61;
  assert.equal((await up({ type: 'REPLY', call: ev, idx: 1 }))[0].status, REPLY_STATUS.SENT);
  const bad = await hub.onIncomingCall({ callSid: 'CA3', from: '+19990000000' });
  assert.equal((await up({ type: 'REPLY', call: bad, idx: 0 }))[0].status, REPLY_STATUS.FAILED);
  assert.equal((await up({ type: 'REPLY', call: 9999, idx: 0 }))[0].status, REPLY_STATUS.NO_CALLER);
  assert.equal(sms.length, 2);
});

test('voicemail: recording then transcript, read in chunks, mark heard and delete', async () => {
  const { hub, pushed, up } = setup();
  hub.setContacts([{ number: ASHA, name: 'Asha Patel' }, { number: 'junk', name: 'x' }]);
  await hub.onIncomingCall({ callSid: 'CA1', from: ASHA });
  assert.equal(hub.onRecording({ callSid: 'CA1', recordingUrl: 'https://api.twilio.com/r/RE1', duration: '1' }), null);
  hub.onRecording({ callSid: 'CA1', recordingUrl: 'https://api.twilio.com/r/RE1', duration: '41' });
  const text = 'Hey it is Asha. '.repeat(20).trim();   // 319 bytes
  hub.onTranscription({ callSid: 'CA1', text });
  assert.deepEqual(pushed.map((m) => m.type), ['EV_CALL', 'EV_VM', 'EV_VM']);
  assert.equal(pushed[1].len, 0);
  assert.equal(pushed[2].len, 319);
  assert.equal(pushed[2].name, 'Asha Patel');
  assert.equal(pushed[2].vm, pushed[1].vm);

  let got = '';
  for (let offset = 0; ;) {
    const [c] = await up({ type: 'VM_GET', vm: pushed[2].vm, offset });
    assert.equal(c.total, 319);
    got += c.chunk;
    offset += Buffer.byteLength(c.chunk);
    if (offset >= c.total) break;
  }
  assert.equal(got, text);

  assert.equal((await up({ type: 'SYNC', last: 0 })).at(-1).unheard, 1);
  assert.equal((await up({ type: 'VM_MARK', vm: pushed[2].vm, action: 1 })).at(-1).unheard, 0);
  assert.equal(hub.listVoicemails().length, 1);
  await up({ type: 'VM_MARK', vm: pushed[2].vm, action: 2 });
  assert.equal(hub.listVoicemails().length, 0);
  assert.equal(hub.recordingUrl(pushed[2].vm), null);
});

test('sync pages through a backlog and duplicate webhooks do not duplicate events', async () => {
  const { hub, up } = setup();
  for (let i = 0; i < 11; i++) await hub.onIncomingCall({ callSid: `CA${i}`, from: ASHA });
  await hub.onIncomingCall({ callSid: 'CA3', from: ASHA });
  const first = await up({ type: 'SYNC', last: 0 });
  assert.equal(first.length, 9);
  const second = await up({ type: 'SYNC', last: first.at(-1).last });
  assert.equal(second.length, 4);
  assert.equal(second.at(-1).last, 11);
});

test('config validation', () => {
  const { hub } = setup();
  assert.throws(() => hub.setConfig({ canned: [] }));
  assert.throws(() => hub.setConfig({ canned: ['ok', '  '] }));
  assert.throws(() => hub.setConfig({ tzMinutes: 5000 }));
  const before = hub.getConfig().cfgVer;
  const cfg = hub.setConfig({ canned: ['One', 'Two'], tzMinutes: -480 });
  assert.equal(cfg.cfgVer, before + 1);
  assert.deepEqual(hub.getConfig().canned, ['One', 'Two']);
});
