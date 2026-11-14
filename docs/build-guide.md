# Build guide

The order matters: each step proves one uncertain thing before the next depends on it.

## 0. Order parts

See [hardware/bom.csv](../hardware/bom.csv). Also useful: a Nordic PPK2 (power measurements),
a USB-UART adapter, a multimeter, calipers, a fine-tip soldering iron.

While you wait, do the accounts in step 4.

## 1. Bench-test the Bluetooth pager (no case, no modem)

1. Run the [hardware verify list](../hardware/README.md#before-you-plug-anything-in-verify-list).
2. Plug the XIAO nRF52840 Plus onto the EN05, the panel ribbon into the EN05, and the battery
   into its socket. That is the whole pager except the motor.
3. Optional 10-minute sanity check: flash one of Seeed's `Seeed_GFX2` Arduino examples for the
   EN05 and this panel. If it draws, the display and wiring are good.
4. Build this firmware with `-DCONFIG_PAGER_LTE=n` and flash it
   ([firmware/README.md](../firmware/README.md)). Expect to fix compile errors in `hw.c` and
   `ble.c` here; those files have never been through a compiler.
5. On the iPhone: Settings > Bluetooth > "Life Pager" > pair, and allow notifications.
6. Call your phone from another one. **Pass**: the pager shows the caller, and Down declines the
   call.

If this works, the core idea works. Everything after is plumbing.

## 2. Add the vibration motor

1. Plug the Grove cable into the Grove Vibration Motor.
2. Solder its three jumper ends to the XIAO's edge pads: signal (yellow) to **D0**, red to
   **3V3**, black to **GND**. If your EN05 revision breaks these pins out on a header, use that
   instead.
3. **Pass**: an incoming call now buzzes.

## 3. Print the case and fit-check

1. Measure the EN05 and update the constants listed in
   [enclosure/README.md](../enclosure/README.md#measure-before-you-print-the-final-copy). The
   board's dimensions are not published, so this step is required, not optional.
2. `python3 enclosure/pager.py` (or `--lte`) must report no interference.
3. Print the shell in draft, check every part drops in and the buttons line up, adjust, then
   print the real set.
4. Assemble as described in the enclosure README.

## 4. Replies and voicemail

1. **Twilio**: create an account, buy a local number with voice + SMS (about $1/month). In
   the US, register for A2P 10DLC as a sole proprietor (or use a verified toll-free number);
   without it carriers block the texts. Budget a few days for approval.
2. **Backend**: deploy [backend/](../backend) with your Twilio credentials and a random
   `APP_TOKEN`. Point the number's voice webhook at `/twilio/voice`.
3. **Test without the phone**: call the Twilio number directly, leave a message, confirm
   `GET /v1/voicemails` shows it with a transcript.
4. **Forwarding**: on AT&T, T-Mobile and most GSM carriers dial `**004*<twilio number>#`
   (cancel with `##004#`). On Verizon dial `*71<number>` (cancel with `*73`). Call your own
   number from another phone, decline it, and confirm it lands on the Twilio greeting.
5. **App**: build [ios/](../ios), enter the server URL and token, edit your replies.
6. **Pass**: call, press Select on the pager, pick a reply; the caller receives the text and
   their voicemail appears on the pager within a couple of minutes.

## 5. Away mode

1. **SIM**: an LTE-M data SIM (1NCE and Hologram both work with the nRF9151). A few MB per
   month is plenty.
2. **Broker**: create an MQTT broker with TLS; set `MQTT_URL` on the backend.
3. **Modem**: flash Serial Modem with the overlay, provision the CA certificate, pass the
   bench check in [firmware/README.md](../firmware/README.md#flash-the-modem-nrf9151-connect-kit).
   Then connect it to the pager: kit P0.02 to XIAO D5, kit P0.03 to XIAO D4, GND to GND, and the
   battery through the Y splitter (see [hardware/README.md](../hardware/README.md#connections)).
4. Rebuild the pager firmware with LTE enabled and your broker settings.
5. **Pass**: turn off Bluetooth on the phone. Within about two minutes the pager's status
   shows `LTE`. Call your number, let it ring out: the pager buzzes with the caller.
6. Measure current in this state with the PPK2 and compare with the
   [power budget](../hardware/README.md#power-budget).

## Running costs (estimates - check current pricing)

| Item | Cost |
|---|---|
| Twilio number | about $1.15 / month |
| Voicemail minutes, transcription, texts | cents per call |
| IoT SIM | $1-2 / month, or a prepaid multi-year plan |
| Backend + broker | free tiers are enough |

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Pager pairs but never shows calls | "Share System Notifications" is off for the pager in iOS Bluetooth settings |
| Display blank | Ribbon inserted the wrong way round or not fully home; 24-pin selection on the EN05 |
| Display shows the previous screen faintly (ghosting) | Partial refresh build-up: see the note in `firmware/src/hw.c` |
| Status never shows `BT` | The app has not subscribed to the pager yet: open it once. This does not depend on the server settings. |
| "Reply queued" and nothing is sent | Forwarding is not active, so the declined call never reached Twilio |
| Texts not delivered (US) | A2P 10DLC registration missing or pending |
| Modem never registers | SIM not activated, antenna not connected, or no LTE-M coverage from that SIM's partner network |
| Modem board gets warm / pager resets when LTE starts | I/O voltage mismatch, or battery too weak for LTE transmit bursts - check both |
| Battery drains in days with the LTE kit fitted, phone nearby | The kit's idle draw; see the power budget in hardware/README.md |
