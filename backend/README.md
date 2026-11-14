# Backend

One small Node 22 service (`mqtt` is its only dependency) that:

- answers calls your carrier forwards to a Twilio number, records and transcribes voicemail;
- sends the canned text replies the pager asks for;
- syncs events and settings with the pager - through the iPhone app over HTTPS when the pager
  is on Bluetooth, and through an MQTT broker when it is on LTE.

```bash
cd backend
```

```bash
npm install
```

```bash
npm test
```

## Deploy

Any host that gives you HTTPS and a persistent disk works (Fly.io, a small VPS, a Raspberry
Pi behind Cloudflare Tunnel). Copy `.env.example` to `.env`, fill it in, then:

```bash
docker build -t life-pager-backend . && docker run -d --env-file .env -v pager-data:/data -p 8080:8080 life-pager-backend
```

In the Twilio console, set the number's "A call comes in" webhook to
`https://<PUBLIC_URL>/twilio/voice` (HTTP POST).

The LTE path needs an MQTT broker with TLS and username/password auth (HiveMQ Cloud or EMQX
Serverless free tiers, or Mosquitto on the same VPS). Create two users, `backend` and
`pager1`, and restrict `pager1` to `pager/pager1/#`.

## API

| Route | Caller | Purpose |
|---|---|---|
| `POST /twilio/voice`, `/twilio/recording`, `/twilio/transcription` | Twilio (signature checked) | call arrives, recording done, transcript done |
| `POST /v1/bridge` | iPhone app | relay pager frames: `{"frames":["hex",...]}` in, same shape out |
| `GET/PUT /v1/config` | iPhone app | owner name, greeting, canned replies, time zone |
| `PUT /v1/contacts` | iPhone app | number -> name table used to label callers |
| `GET /v1/voicemails`, `GET /v1/voicemails/:id/audio`, `POST` (heard), `DELETE` | iPhone app | voicemail list and playback |

Everything under `/v1` needs `Authorization: Bearer $APP_TOKEN`.

## Rules the reply logic enforces (see `test/hub.test.mjs`)

- A reply only ever goes to a number that called you.
- "Reply to the call ringing now" matches a call that reached voicemail in the last 30 s, or
  waits up to 2 minutes for it. It never falls back to an earlier caller.
- One text per caller per minute. Withheld numbers get nothing.
