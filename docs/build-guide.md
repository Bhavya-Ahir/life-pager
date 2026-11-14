# Build guide

The order matters: each step proves one uncertain thing before the next depends on it.

## 0. Order parts

See [hardware/bom.csv](../hardware/bom.csv). Also useful: a Nordic PPK2 (power measurements),
a USB-UART adapter, a multimeter, calipers.

While you wait, do the accounts in step 4.

## 1. Bench-test the Bluetooth pager (no case, no modem)

1. Run the [hardware verify list](../hardware/README.md#before-you-solder-verify-list).
2. On a breadboard wire only: XIAO, display (3 wires + power), three buttons, motor driver.
3. Build the firmware with `-DCONFIG_PAGER_LTE=n` and flash it
   ([firmware/README.md](../firmware/README.md)). Expect to fix compile errors in `hw.c`,
   `ble.c` here - those files have never been through a compiler.
4. On the iPhone: Settings > Bluetooth > "Life Pager" > pair, and allow notifications.
5. Call your phone from another one. **Pass**: the pager buzzes and shows the caller; Down
   declines the call.

If this works, the core idea works. Everything after is plumbing.

## 2. Print the case and fit-check

1. Measure your parts and update the constants listed in
   [enclosure/README.md](../enclosure/README.md#measure-before-you-print-the-final-copy).
2. `python3 enclosure/pager.py` must report no interference.
3. Print the shell in draft, check every part drops in, adjust, then print the real set.

## 3. Solder Rev A

Follow [hardware/schematic.svg](../hardware/schematic.svg). Practical order:

1. Button strip: three switches on a 13 x 32 mm perfboard strip at 10 mm pitch, one common
   ground wire, three signal wires.
2. Driver scrap board: Q1, D1, R1, R2 (motor) and Q2, Q3, R3, R4, R7, C1 (modem switch).
3. XIAO underside: wires on BAT+, BAT- and NFC1 **before** anything else is attached.
4. Display header wires, then join everything at the XIAO.
5. Battery last. Check for 3.3 V on the XIAO's 3V3 pin before closing up.

Use 30 AWG silicone wire and keep each run a little longer than needed; the tray has slots
for the display and button wires.

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
| Display blank | CS polarity (it is active high), or the breakout's pin order differs from the netlist |
| Status never shows `BT` | The app has not subscribed: open it once; check the server URL/token do not matter for this - the bridge comes up even without a server |
| "Reply queued" and nothing is sent | Forwarding is not active, so the declined call never reached Twilio |
| Texts not delivered (US) | A2P 10DLC registration missing or pending |
| Modem never registers | SIM not activated, antenna not connected, or no LTE-M coverage from that SIM's partner network |
| Modem board gets warm / pager resets when LTE starts | I/O voltage mismatch, or battery too weak for LTE transmit bursts - check both |
