# Pager protocol v1

One protocol, two transports. The pager and the backend exchange small binary **frames**:

| Transport | When | Framing |
|---|---|---|
| Bluetooth bridge | pager is connected to the iPhone app | one frame per GATT write / notification; the app relays them to `POST /v1/bridge` as hex without reading them |
| LTE-M | pager is on its own | one frame per MQTT message, as lowercase hex text, on `pager/<id>/up` and `pager/<id>/down` (QoS 1) |

Reference implementations: [`firmware/src/proto.c`](../firmware/src/proto.c) and
[`backend/src/proto.mjs`](../backend/src/proto.mjs). Both are tested against
[`protocol/vectors.txt`](../protocol/vectors.txt); regenerate it with `npm run vectors` in
`backend/` after any change.

## Frame layout

```
byte 0   type
byte 1   seq      (sender's counter, for logs; not acknowledged)
byte 2.. fields, in the order listed below
```

Integers are little-endian. `str:N` is one length byte followed by that many UTF-8 bytes, at
most N. A frame is at most 180 bytes, so it fits one BLE packet at iOS's usual 185-byte MTU.
Frames with an unknown type, missing bytes or trailing bytes are dropped.

## Pager to cloud

| Type | Name | Fields | Meaning |
|---|---|---|---|
| 0x01 | HELLO | proto u8, fw_major u8, fw_minor u8, batt u8, link u8 (0 BLE, 1 LTE), cfg_ver u16, last_event u32 | sent whenever a link comes up |
| 0x02 | SYNC | last_event u32 | "send me events newer than this" |
| 0x03 | REPLY | call_event u32, canned_idx u8 | text the caller. `call_event` 0 = the call ringing right now |
| 0x04 | VM_GET | vm_id u32, offset u16 | request transcript bytes from `offset` |
| 0x05 | VM_MARK | vm_id u32, action u8 (1 heard, 2 delete) | |

## Cloud to pager

| Type | Name | Fields | Meaning |
|---|---|---|---|
| 0x81 | TIME | unix u32, tz_min i16 | clock and time zone |
| 0x82 | CANNED | cfg_ver u16, idx u8, count u8, text str:160 | one preset reply; sent when HELLO's cfg_ver is stale |
| 0x83 | EV_CALL | event u32, ts u32, flags u8, number str:20, name str:32 | a call reached voicemail |
| 0x84 | EV_VM | event u32, vm_id u32, ts u32, duration u16, text_len u16, number str:20, name str:32 | voicemail recorded; sent again (same vm_id, new event) when the transcript is ready |
| 0x85 | VM_TEXT | vm_id u32, offset u16, total u16, chunk str:160 | transcript bytes |
| 0x86 | REPLY_RESULT | call_event u32, status u8 | 0 sent, 1 queued, 2 no number to text, 3 failed |
| 0x87 | SYNC_DONE | last_event u32, unheard u8 | end of a HELLO / SYNC answer |

## Sequences

**Link comes up**: `HELLO` -> `TIME`, `CANNED` x n (only if changed), up to 8 events,
`SYNC_DONE`. If any events arrived the pager sends `SYNC` again until one comes back empty.

**Reply to the ringing call** (phone nearby): the pager declines over ANCS and sends
`REPLY call_event=0`. The declined call is forwarded by the carrier and reaches the backend a
few seconds later; the backend sends the text then and answers `REPLY_RESULT`. If the reply
arrives first it is held for up to 2 minutes (`status 1`), then dropped. It is never applied
to an older call.

**Voicemail**: `EV_VM` (text_len 0) when the recording ends, `EV_VM` again when the
transcript is ready. Opening it sends `VM_GET` repeatedly until `offset` reaches `total`,
then `VM_MARK heard`. The pager keeps the first 640 bytes.

**Push**: on LTE the backend publishes `EV_CALL` / `EV_VM` as they happen. On Bluetooth there
is no push channel through iOS, so the pager polls with `SYNC` 20 s, 80 s and 140 s after a
ring ends.

## Bluetooth service

| | UUID | Properties |
|---|---|---|
| Service | `7A1E0001-5B0C-4C1E-9F0A-6C1F5E9A2B10` | |
| RX (app to pager) | `7A1E0002-...` | write, write without response; encrypted link required |
| TX (pager to app) | `7A1E0003-...` | notify; subscribing requires an encrypted link |

The pager treats "TX notifications enabled" as "the bridge is up".
