// HTTP + MQTT front end for the Hub. Configuration is by environment variable, see .env.example.
import { createServer } from 'node:http';
import { timingSafeEqual } from 'node:crypto';
import { Readable } from 'node:stream';
import { Hub } from './hub.mjs';
import { fromHex, toHex } from './proto.mjs';
import { twilioClient, validSignature, voicemailTwiml } from './twilio.mjs';

const MAX_BODY = 256 * 1024;

function sameToken(given, expected) {
  const a = Buffer.from(String(given ?? ''));
  const b = Buffer.from(String(expected));
  return a.length === b.length && timingSafeEqual(a, b);
}

async function readBody(req) {
  const chunks = [];
  let size = 0;
  for await (const c of req) {
    size += c.length;
    if (size > MAX_BODY) throw Object.assign(new Error('body too large'), { status: 413 });
    chunks.push(c);
  }
  return Buffer.concat(chunks).toString('utf8');
}

/**
 * @param {object} env
 * @param {object} [deps] test seams: { hub, twilio, mqtt }
 */
export function createApp(env, deps = {}) {
  for (const k of ['PUBLIC_URL', 'APP_TOKEN', 'TWILIO_AUTH_TOKEN']) {
    if (!env[k]) throw new Error(`${k} is required`);
  }
  if (env.APP_TOKEN.length < 24) throw new Error('APP_TOKEN must be at least 24 characters');
  const baseUrl = env.PUBLIC_URL.replace(/\/$/, '');
  const twilio = deps.twilio ?? twilioClient({
    accountSid: env.TWILIO_ACCOUNT_SID, authToken: env.TWILIO_AUTH_TOKEN, fromNumber: env.TWILIO_NUMBER });
  const topic = (dir) => `pager/${env.PAGER_ID ?? 'pager1'}/${dir}`;
  const mqtt = deps.mqtt ?? null;
  const hub = deps.hub ?? new Hub({
    dbPath: env.DB_PATH ?? 'pager.db',
    sendSms: (to, body) => twilio.sendSms(to, body),
    // MQTT payloads are frames as hex text: the pager's modem is driven by text AT commands
    push: (frames) => { for (const f of frames) mqtt?.publish(topic('down'), toHex(f), { qos: 1 }); },
  });

  if (mqtt) {
    mqtt.subscribe(topic('up'), { qos: 1 });
    mqtt.on('message', async (t, payload) => {
      if (t !== topic('up')) return;
      try {
        const hex = payload.toString('ascii').trim().toLowerCase();
        if (!/^([0-9a-f]{2}){2,180}$/.test(hex)) throw new Error('not a hex frame');
        for (const f of await hub.handleUp(fromHex(hex))) {
          mqtt.publish(topic('down'), toHex(f), { qos: 1 });
        }
      } catch (e) {
        console.error('bad frame from pager:', e.message);
      }
    });
  }

  const json = (res, status, body) => {
    res.writeHead(status, { 'content-type': 'application/json' });
    res.end(JSON.stringify(body));
  };

  async function twilioForm(req, res, path) {
    const params = Object.fromEntries(new URLSearchParams(await readBody(req)));
    if (!validSignature(env.TWILIO_AUTH_TOKEN, baseUrl + path, params, req.headers['x-twilio-signature'])) {
      json(res, 403, { error: 'bad signature' });
      return null;
    }
    return params;
  }

  async function route(req, res) {
    const url = new URL(req.url, 'http://x');
    const path = url.pathname;
    const key = `${req.method} ${path}`;

    if (key === 'GET /healthz') return json(res, 200, { ok: true });

    // ---------------------------------------------------------------- Twilio webhooks
    if (key === 'POST /twilio/voice') {
      const p = await twilioForm(req, res, path);
      if (!p) return;
      await hub.onIncomingCall({ callSid: p.CallSid, from: p.From });
      res.writeHead(200, { 'content-type': 'text/xml' });
      return res.end(voicemailTwiml({ greeting: hub.getConfig().greeting, baseUrl }));
    }
    if (key === 'POST /twilio/recording') {
      const p = await twilioForm(req, res, path);
      if (!p) return;
      hub.onRecording({ callSid: p.CallSid, recordingUrl: p.RecordingUrl, duration: p.RecordingDuration });
      return json(res, 200, {});
    }
    if (key === 'POST /twilio/transcription') {
      const p = await twilioForm(req, res, path);
      if (!p) return;
      if (p.TranscriptionStatus === 'completed') {
        hub.onTranscription({ callSid: p.CallSid, text: p.TranscriptionText });
      }
      return json(res, 200, {});
    }

    // ---------------------------------------------------------------- everything else needs the app token
    const bearer = (req.headers.authorization ?? '').replace(/^Bearer /, '');
    if (!sameToken(bearer, env.APP_TOKEN)) return json(res, 401, { error: 'unauthorized' });

    // The iPhone app relays opaque pager frames here when the pager is on Bluetooth.
    if (key === 'POST /v1/bridge') {
      const body = JSON.parse(await readBody(req));
      if (!Array.isArray(body.frames) || body.frames.length > 16) return json(res, 400, { error: 'frames' });
      const out = [];
      for (const hex of body.frames) {
        if (typeof hex !== 'string' || !/^([0-9a-f]{2}){2,180}$/.test(hex)) {
          return json(res, 400, { error: 'bad frame' });
        }
        let frames;
        try {
          frames = await hub.handleUp(fromHex(hex));
        } catch (e) {
          return json(res, 400, { error: `rejected frame: ${e.message}` });
        }
        for (const f of frames) out.push(toHex(f));
      }
      return json(res, 200, { frames: out });
    }
    if (key === 'GET /v1/config') return json(res, 200, hub.getConfig());
    if (key === 'PUT /v1/config') {
      try {
        return json(res, 200, hub.setConfig(JSON.parse(await readBody(req))));
      } catch (e) {
        return json(res, 400, { error: e.message });
      }
    }
    if (key === 'PUT /v1/contacts') {
      const body = JSON.parse(await readBody(req));
      if (!Array.isArray(body.contacts)) return json(res, 400, { error: 'contacts' });
      hub.setContacts(body.contacts);
      return json(res, 200, {});
    }
    if (key === 'GET /v1/voicemails') return json(res, 200, { voicemails: hub.listVoicemails() });

    const vm = path.match(/^\/v1\/voicemails\/(\d+)(\/audio)?$/);
    if (vm && req.method === 'GET' && vm[2]) {
      const recording = hub.recordingUrl(Number(vm[1]));
      if (!recording) return json(res, 404, { error: 'not found' });
      const upstream = await twilio.fetchRecording(recording);
      if (!upstream.ok) return json(res, 502, { error: 'recording unavailable' });
      res.writeHead(200, { 'content-type': 'audio/mpeg' });
      return Readable.fromWeb(upstream.body).pipe(res);
    }
    if (vm && !vm[2] && (req.method === 'DELETE' || req.method === 'POST')) {
      const ok = hub.markVoicemail(Number(vm[1]), req.method === 'DELETE' ? 2 : 1);
      return json(res, ok ? 200 : 404, {});
    }
    return json(res, 404, { error: 'not found' });
  }

  const server = createServer((req, res) => {
    route(req, res).catch((e) => {
      if (!res.headersSent) json(res, e.status ?? (e instanceof SyntaxError ? 400 : 500), { error: 'request failed' });
      else res.destroy();
      if (!e.status && !(e instanceof SyntaxError)) console.error(e);
    });
  });
  return { server, hub };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const env = process.env;
  let mqtt = null;
  if (env.MQTT_URL) {
    const { connect } = await import('mqtt');
    mqtt = connect(env.MQTT_URL, { username: env.MQTT_USER, password: env.MQTT_PASS, clientId: 'pager-backend' });
    mqtt.on('error', (e) => console.error('mqtt:', e.message));
  }
  const { server } = createApp(env, { mqtt });
  server.listen(Number(env.PORT ?? 8080), () => console.log(`pager backend on :${env.PORT ?? 8080}`));
}
