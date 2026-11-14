import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/server.mjs';
import { Hub } from '../src/hub.mjs';
import { signature } from '../src/twilio.mjs';
import { encode, decode, toHex, fromHex } from '../src/proto.mjs';

const env = { PUBLIC_URL: 'https://pager.test', APP_TOKEN: 'a'.repeat(32), TWILIO_AUTH_TOKEN: 'secret' };

async function start() {
  const sms = [];
  const hub = new Hub({ sendSms: async (to, body) => sms.push({ to, body }) });
  const twilio = { fetchRecording: async () => new Response('mp3-bytes', { status: 200 }) };
  const { server } = createApp(env, { hub, twilio });
  await new Promise((r) => server.listen(0, r));
  const base = `http://127.0.0.1:${server.address().port}`;
  const twilioPost = (path, params, sig) => fetch(base + path, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded',
      'x-twilio-signature': sig ?? signature('secret', env.PUBLIC_URL + path, params) },
    body: new URLSearchParams(params) });
  const api = (method, path, body, token = env.APP_TOKEN) => fetch(base + path, {
    method, headers: { authorization: `Bearer ${token}` }, body: body && JSON.stringify(body) });
  return { server, hub, sms, base, twilioPost, api };
}

test('end to end over HTTP: forwarded call, voicemail, bridge sync, reply, audio', async (t) => {
  const s = await start();
  t.after(() => s.server.close());

  let res = await s.twilioPost('/twilio/voice', { CallSid: 'CA1', From: '+14155550123' }, 'forged');
  assert.equal(res.status, 403);
  res = await s.twilioPost('/twilio/voice', { CallSid: 'CA1', From: '+14155550123' });
  assert.equal(res.status, 200);
  assert.match(await res.text(), /<Record[^>]+transcribeCallback="https:\/\/pager.test\/twilio\/transcription"/);
  await s.twilioPost('/twilio/recording',
    { CallSid: 'CA1', RecordingUrl: 'https://api.twilio.com/r/RE1', RecordingDuration: '12' });
  await s.twilioPost('/twilio/transcription',
    { CallSid: 'CA1', TranscriptionStatus: 'completed', TranscriptionText: 'Call me back' });

  assert.equal((await s.api('GET', '/v1/voicemails', null, 'wrong')).status, 401);
  assert.equal((await fetch(s.base + '/v1/config')).status, 401);

  const hello = toHex(encode({ type: 'HELLO', seq: 1, proto: 1, fwMajor: 0, fwMinor: 1, batt: 80, link: 0, cfg: 0, last: 0 }));
  res = await s.api('POST', '/v1/bridge', { frames: [hello] });
  const frames = (await res.json()).frames.map((h) => decode(fromHex(h)));
  assert.deepEqual(frames.map((m) => m.type),
    ['TIME', 'CANNED', 'CANNED', 'CANNED', 'CANNED', 'EV_CALL', 'EV_VM', 'EV_VM', 'SYNC_DONE']);

  const reply = toHex(encode({ type: 'REPLY', seq: 2, call: frames[5].event, idx: 0 }));
  res = await s.api('POST', '/v1/bridge', { frames: [reply] });
  assert.equal(decode(fromHex((await res.json()).frames[0])).status, 0);
  assert.equal(s.sms[0].to, '+14155550123');

  assert.equal((await s.api('POST', '/v1/bridge', { frames: ['zz'] })).status, 400);
  assert.equal((await s.api('POST', '/v1/bridge', { frames: ['7f00'] })).status, 400);

  const list = (await (await s.api('GET', '/v1/voicemails')).json()).voicemails;
  assert.equal(list.length, 1);
  assert.equal(list[0].transcript, 'Call me back');
  res = await s.api('GET', `/v1/voicemails/${list[0].id}/audio`);
  assert.equal(res.headers.get('content-type'), 'audio/mpeg');
  assert.equal(await res.text(), 'mp3-bytes');

  assert.equal((await s.api('PUT', '/v1/config', { canned: [] })).status, 400);
  assert.equal((await s.api('PUT', '/v1/config', { canned: ['Back soon'] })).status, 200);
  assert.equal((await s.api('DELETE', `/v1/voicemails/${list[0].id}`)).status, 200);
  assert.equal((await s.api('GET', `/v1/voicemails/${list[0].id}/audio`)).status, 404);
});

test('refuses to start without secrets', () => {
  assert.throws(() => createApp({ ...env, APP_TOKEN: 'short' }), /24 characters/);
  assert.throws(() => createApp({ PUBLIC_URL: 'x' }), /required/);
});

test('LTE path: hex frames in on the up topic, hex frames out on the down topic', async (t) => {
  const { EventEmitter } = await import('node:events');
  const mqtt = new EventEmitter();
  const published = [];
  mqtt.subscribe = (topic) => { mqtt.subscribed = topic; };
  mqtt.publish = (topic, payload) => published.push({ topic, payload });
  const hub = new Hub({ sendSms: async () => {}, push: (frames) => frames.forEach((f) => mqtt.publish('pager/pager1/down', toHex(f))) });
  const { server } = createApp(env, { hub, twilio: {}, mqtt });
  t.after(() => server.close());
  assert.equal(mqtt.subscribed, 'pager/pager1/up');

  const done = () => new Promise((r) => setTimeout(r, 20));
  mqtt.emit('message', 'pager/pager1/up', Buffer.from(toHex(encode({ type: 'SYNC', seq: 1, last: 0 }))));
  await done();
  assert.equal(published.length, 1);
  assert.equal(published[0].topic, 'pager/pager1/down');
  assert.equal(decode(fromHex(published[0].payload)).type, 'SYNC_DONE');

  mqtt.emit('message', 'pager/pager1/up', Buffer.from('not hex'));       // ignored, no crash
  mqtt.emit('message', 'pager/other/up', Buffer.from('0201'));           // wrong topic
  await done();
  assert.equal(published.length, 1);

  await hub.onIncomingCall({ callSid: 'CA9', from: '+14155550123' });    // pushed without being asked
  assert.equal(decode(fromHex(published[1].payload)).type, 'EV_CALL');
});
