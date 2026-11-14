// The small part of Twilio this project needs, without the SDK.
import { createHmac, timingSafeEqual } from 'node:crypto';

const xml = (s) => String(s).replace(/[<>&'"]/g, (c) => (
  { '<': '&lt;', '>': '&gt;', '&': '&amp;', "'": '&apos;', '"': '&quot;' }[c]));

/** TwiML that plays the greeting and records a voicemail (max 2 min, transcribed). */
export function voicemailTwiml({ greeting, baseUrl }) {
  return `<?xml version="1.0" encoding="UTF-8"?>
<Response>
  <Say>${xml(greeting)}</Say>
  <Record maxLength="120" timeout="5" playBeep="true" transcribe="true"
          transcribeCallback="${xml(baseUrl)}/twilio/transcription"
          recordingStatusCallback="${xml(baseUrl)}/twilio/recording"
          recordingStatusCallbackEvent="completed"/>
  <Hangup/>
</Response>`;
}

/** Twilio request signature: base64(HMAC-SHA1(authToken, url + sorted key/value pairs)). */
export function signature(authToken, url, params) {
  const data = Object.keys(params).sort().reduce((acc, k) => acc + k + params[k], url);
  return createHmac('sha1', authToken).update(data, 'utf8').digest('base64');
}

export function validSignature(authToken, url, params, header) {
  if (!header) return false;
  const a = Buffer.from(signature(authToken, url, params));
  const b = Buffer.from(String(header));
  return a.length === b.length && timingSafeEqual(a, b);
}

export function twilioClient({ accountSid, authToken, fromNumber, fetchImpl = fetch }) {
  const auth = `Basic ${Buffer.from(`${accountSid}:${authToken}`).toString('base64')}`;
  return {
    async sendSms(to, body) {
      const res = await fetchImpl(
        `https://api.twilio.com/2010-04-01/Accounts/${accountSid}/Messages.json`, {
          method: 'POST',
          headers: { authorization: auth, 'content-type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({ To: to, From: fromNumber, Body: body }),
        });
      if (!res.ok) throw new Error(`Twilio SMS failed: ${res.status} ${await res.text()}`);
    },
    /** Recordings are private to the account; the app plays them through this proxy. */
    fetchRecording(recordingUrl) {
      if (!recordingUrl.startsWith('https://api.twilio.com/')) throw new Error('unexpected recording host');
      return fetchImpl(`${recordingUrl}.mp3`, { headers: { authorization: auth } });
    },
  };
}
