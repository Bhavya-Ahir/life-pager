/*
 * Life Pager firmware entry point: owns the model and decides which link carries traffic.
 *
 *   phone app relaying over Bluetooth  -> use it, keep the LTE modem powered off
 *   no phone for PAGER_AWAY_DELAY_S    -> power the modem, connect MQTT over LTE-M
 *   phone comes back                   -> switch back, power the modem off
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(pager, LOG_LEVEL_INF);

K_MSGQ_DEFINE(evt_q, sizeof(struct app_evt), 12, 4);

static struct pager pager;
static bool ble_up, lte_up;
static int64_t ble_lost_at;

void app_post(const struct app_evt *e)
{
	if (k_msgq_put(&evt_q, e, K_NO_WAIT) != 0) {
		LOG_WRN("event queue full, dropped type %u", e->type);
	}
}

static uint32_t now_s(void)
{
	return (uint32_t)(k_uptime_get() / 1000);
}

static void io_send(const struct proto_msg *m, void *ctx)
{
	uint8_t buf[PROTO_MAX_FRAME];
	int len = proto_encode(m, buf, sizeof(buf));
	int err;

	ARG_UNUSED(ctx);
	if (len < 0) {
		return;
	}
	err = ble_up ? ble_send(buf, len) : modem_send(buf, len);
	if (err) {
		LOG_WRN("send type 0x%02x failed: %d", m->type, err);
	}
}

static void io_haptic(enum haptic h, void *ctx)
{
	ARG_UNUSED(ctx);
	hw_haptic(h);
}

static void io_decline(void *ctx)
{
	ARG_UNUSED(ctx);
	ble_decline_call();
}

static void update_link(void)
{
	static int8_t current = -1;         /* -1 none, else enum proto_link */
	int8_t want = ble_up ? LINK_BLE_BRIDGE : (lte_up ? LINK_LTE : -1);

	if (want == current) {
		return;
	}
	current = want;
	pager_link_changed(&pager, want >= 0, want >= 0 ? want : 0, now_s());
}

static void handle(const struct app_evt *e)
{
	struct proto_msg m;

	switch (e->type) {
	case EVT_BUTTON:
		pager_on_button(&pager, e->arg, now_s());
		break;
	case EVT_RING:
		pager_on_ring(&pager, e->text, now_s());
		break;
	case EVT_RING_END:
		pager_on_ring_end(&pager, now_s());
		break;
	case EVT_FRAME:
		if (proto_decode(e->data, e->len, &m) == 0 && (m.type & 0x80)) {
			pager_on_frame(&pager, &m, now_s());
		}
		break;
	case EVT_BLE_BRIDGE:
		ble_up = e->arg;
		if (ble_up) {
			modem_request(false);
		} else {
			ble_lost_at = k_uptime_get();
		}
		update_link();
		break;
	case EVT_LTE:
		lte_up = e->arg;
		update_link();
		break;
	}
}

int main(void)
{
	const struct pager_io io = { io_send, io_haptic, io_decline, NULL };
	struct app_evt e;
	int64_t next_tick = 0;
	uint32_t ticks = 0;

	pager_init(&pager, &io);
	if (hw_init() || ble_init() || modem_init()) {
		LOG_ERR("init failed");
	}
	ble_lost_at = k_uptime_get();

	for (;;) {
		/* Sleep until an event or the next minute/second boundary we care about. The
		 * 1 s tick only runs while something on screen can change; otherwise 10 s. */
		k_timeout_t wait = K_MSEC(MAX(next_tick - k_uptime_get(), 0));

		if (k_msgq_get(&evt_q, &e, wait) == 0) {
			handle(&e);
		}
		if (k_uptime_get() >= next_tick) {
			bool busy = pager.screen != SCR_HOME || pager.toast_until != 0;

			next_tick = k_uptime_get() + (busy ? 1000 : 10000);
			pager_tick(&pager, now_s());
			if ((ticks++ % 30) == 0) {
				pager_set_battery(&pager, hw_battery_pct());
			}
			if (IS_ENABLED(CONFIG_PAGER_LTE) && !ble_up &&
			    k_uptime_get() - ble_lost_at > CONFIG_PAGER_AWAY_DELAY_S * 1000LL) {
				modem_request(true);
			}
		}
		if (pager.dirty) {
			pager.dirty = false;
			hw_draw(&pager);
		}
	}
	return 0;
}
