/* Glue between the Zephyr-facing modules and the model. Everything funnels through one
 * event queue so the model is only ever touched from the main thread. */
#ifndef PAGER_APP_H
#define PAGER_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model.h"

enum app_evt_type {
	EVT_BUTTON,         /* arg = enum button */
	EVT_RING,           /* text = caller */
	EVT_RING_END,
	EVT_FRAME,          /* data/len = one protocol frame from the cloud */
	EVT_BLE_BRIDGE,     /* arg = 1 when the phone app is relaying, 0 when it stopped */
	EVT_LTE,            /* arg = 1 when MQTT is connected, 0 when it dropped */
};

struct app_evt {
	uint8_t type;
	uint8_t arg;
	uint8_t len;
	union {
		uint8_t data[PROTO_MAX_FRAME];
		char text[PROTO_MAX_NAME + 1];
	};
};

void app_post(const struct app_evt *e);     /* safe from any thread; drops if the queue is full */

/* hw.c */
int hw_init(void);
void hw_haptic(enum haptic h);
uint8_t hw_battery_pct(void);
void hw_draw(const struct pager *p);

/* ble.c */
int ble_init(void);
bool ble_bridge_ready(void);
int ble_send(const uint8_t *frame, size_t len);
void ble_decline_call(void);

/* modem.c */
int modem_init(void);
void modem_request(bool on);                /* power up + connect, or disconnect + power down */
bool modem_connected(void);
int modem_send(const uint8_t *frame, size_t len);

#endif
