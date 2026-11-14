# Firmware

Zephyr / nRF Connect SDK application for the XIAO nRF52840 Plus on Seeed's EN05 e-paper board,
plus the configuration for the optional nRF9151 kit that acts as its LTE modem. All pin
assignments are in [`boards/xiao_nrf52840_plus.overlay`](boards/xiao_nrf52840_plus.overlay).

| File | What it does | Tested how |
|---|---|---|
| `src/proto.c` | Wire protocol encode/decode ([docs/protocol.md](../docs/protocol.md)) | Host tests, sanitizers, shared vectors with the backend, 200k random frames |
| `src/model.c` | Inbox, replies, voicemail reader and the whole UI state machine, rendered to a 25x12 text grid | Host tests that drive buttons/frames and assert on the screen text |
| `src/main.c` | Event loop and link selection (Bluetooth bridge vs LTE) | **Not compiled here** |
| `src/hw.c` | EN05 buttons, Grove motor patterns, battery gauge, e-paper via CFB | **Not compiled here** |
| `src/ble.c` | ANCS client (call alerts, decline) and the bridge GATT service | **Not compiled here** |
| `src/modem.c` | AT-command driver for the nRF9151 Serial Modem (MQTT over LTE-M) | **Not compiled here** |

Be clear-eyed about that last column: this repository was produced without an nRF Connect SDK
toolchain or hardware. The hardware-independent half is tested; the Zephyr-facing half is
written against the NCS APIs from memory of the `peripheral_ancs_client` sample and the Serial
Modem documentation. The display description follows Zephyr's own
`waveshare_epaper_gdeh0154a07` overlay (the same SSD1681 200x200 controller), and the board target
`xiao_nrf52840_plus` exists upstream. Even so, the code has never been through a compiler and
**will need fix-ups on first build** (API names drift between NCS releases). Each of those files
says at the top what to verify.

## Host tests (no hardware, no SDK)

```bash
make -C firmware/tests
```

## Build and flash the pager (XIAO nRF52840 Plus on the EN05)

Install nRF Connect SDK v3.x (the nRF Connect for VS Code extension is the easy route), then:

```bash
cd firmware
```

```bash
printf 'CONFIG_PAGER_MQTT_HOST="broker.example.com"\nCONFIG_PAGER_MQTT_PASS="change-me"\n' > private.conf
```

```bash
west build -b xiao_nrf52840_plus -- -DEXTRA_CONF_FILE=private.conf
```

Double-tap the reset button (on the EN05's edge) so the XIAO mounts as a USB drive, then copy
`build/zephyr/zephyr.uf2` onto it.

For a Bluetooth-only build (no nRF9151 fitted) add `-DCONFIG_PAGER_LTE=n`.

If you would rather start in Arduino: Seeed's `Seeed_GFX2` library has ready examples for the
EN05 and this panel. They are a good 10-minute check that the display and buttons work before
you build this firmware.

## Flash the modem (nRF9151 Connect Kit)

The kit runs Nordic's stock **Serial Modem** application - no custom code. Build it from the
Serial Modem add-on for the Connect Kit's board target with `modem/pager_modem.overlay`
applied, flash it through the kit's own USB port, then provision the broker's CA certificate
into security tag 4242 (`AT%CMNG=0,4242,0,"-----BEGIN CERTIFICATE-----..."`).

Bench check before wiring it to the XIAO: with a USB-UART adapter on P0.02/P0.03, `AT` must
answer `OK`, and `AT+CFUN=1` followed by `AT+CEREG?` must reach `1` or `5` with your SIM.

## Buttons

The EN05's three side buttons are KEY1 = Up, KEY2 = Select, KEY3 = Down (swap the codes in the
overlay if you prefer another order).

| Button | Home | Ringing | Lists | Voicemail text |
|---|---|---|---|---|
| Up | open inbox | silence | move up | scroll up |
| Select | open inbox | decline and pick a reply | choose | back |
| Down | open inbox | decline | move down | scroll down |
