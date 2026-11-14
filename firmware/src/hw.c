/* Buttons, vibration motor, battery gauge and the Sharp memory LCD. */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_DECLARE(pager);

#define USER DT_PATH(zephyr_user)
#define FONT_W 8
#define FONT_H 16

static const struct gpio_dt_spec haptic_pin = GPIO_DT_SPEC_GET(USER, haptic_gpios);
static const struct gpio_dt_spec vbat_en = GPIO_DT_SPEC_GET(USER, vbat_en_gpios);
static const struct gpio_dt_spec charge_fast = GPIO_DT_SPEC_GET(USER, charge_fast_gpios);
static const struct adc_dt_spec vbat_adc = ADC_DT_SPEC_GET(USER);
static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* ------------------------------------------------------------------ buttons */
static void on_input(struct input_event *evt, void *user_data)
{
	struct app_evt e = { .type = EVT_BUTTON };

	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return;                               /* act on press, ignore release */
	}
	switch (evt->code) {
	case INPUT_KEY_UP: e.arg = BTN_UP; break;
	case INPUT_KEY_ENTER: e.arg = BTN_SELECT; break;
	case INPUT_KEY_DOWN: e.arg = BTN_DOWN; break;
	default: return;
	}
	app_post(&e);
}
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);      /* gpio-keys debounces for us */

/* ------------------------------------------------------------------ haptics */
/* Patterns are on/off durations in ms, terminated by 0. RING repeats until stopped. */
static const uint16_t pat_tick[] = { 40, 0 };
static const uint16_t pat_alert[] = { 250, 150, 250, 150, 250, 0 };
static const uint16_t pat_ring[] = { 400, 300, 400, 1500, 0 };
static const uint16_t *pattern;
static uint8_t pat_pos, ring_cycles;
static bool pat_repeat;

static void haptic_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(haptic_work, haptic_step);

static void haptic_step(struct k_work *work)
{
	ARG_UNUSED(work);
	if (pattern == NULL || pattern[pat_pos] == 0) {
		gpio_pin_set_dt(&haptic_pin, 0);
		if (pattern != NULL && pat_repeat && ++ring_cycles < 12) {   /* ~30 s at most */
			pat_pos = 0;
			k_work_reschedule(&haptic_work, K_NO_WAIT);
		} else {
			pattern = NULL;
		}
		return;
	}
	gpio_pin_set_dt(&haptic_pin, (pat_pos % 2) == 0);
	k_work_reschedule(&haptic_work, K_MSEC(pattern[pat_pos++]));
}

void hw_haptic(enum haptic h)
{
	k_work_cancel_delayable(&haptic_work);
	gpio_pin_set_dt(&haptic_pin, 0);
	pat_pos = 0;
	ring_cycles = 0;
	pat_repeat = (h == HAPTIC_RING);
	switch (h) {
	case HAPTIC_TICK: pattern = pat_tick; break;
	case HAPTIC_ALERT: pattern = pat_alert; break;
	case HAPTIC_RING: pattern = pat_ring; break;
	default: pattern = NULL; return;
	}
	k_work_reschedule(&haptic_work, K_NO_WAIT);
}

/* ------------------------------------------------------------------ battery */
uint8_t hw_battery_pct(void)
{
	/* The XIAO divides VBAT by 1510k / 510k before AIN7. Open-circuit LiPo curve, coarse. */
	static const struct { uint16_t mv; uint8_t pct; } curve[] = {
		{ 4150, 100 }, { 4000, 85 }, { 3850, 65 }, { 3750, 45 }, { 3680, 25 }, { 3550, 10 }, { 3300, 0 },
	};
	int16_t raw = 0;
	struct adc_sequence seq = { .buffer = &raw, .buffer_size = sizeof(raw) };
	int32_t mv;

	gpio_pin_set_dt(&vbat_en, 1);
	k_msleep(2);
	adc_sequence_init_dt(&vbat_adc, &seq);
	if (adc_read_dt(&vbat_adc, &seq) != 0) {
		gpio_pin_set_dt(&vbat_en, 0);
		return 0;
	}
	gpio_pin_set_dt(&vbat_en, 0);
	mv = raw;
	adc_raw_to_millivolts_dt(&vbat_adc, &mv);
	mv = mv * (1000 + 510) / 510;
	for (size_t i = 0; i < ARRAY_SIZE(curve); i++) {
		if (mv >= curve[i].mv) {
			if (i == 0) {
				return 100;
			}
			return curve[i].pct + (curve[i - 1].pct - curve[i].pct) * (mv - curve[i].mv) /
						      (curve[i - 1].mv - curve[i].mv);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ display */
void hw_draw(const struct pager *p)
{
	char rows[UI_ROWS][UI_COLS + 1];
	int invert;

	pager_render(p, rows, &invert);
	cfb_framebuffer_clear(display, false);
	for (int r = 0; r < UI_ROWS; r++) {
		if (rows[r][0]) {
			cfb_print(display, rows[r], 0, r * FONT_H);
		}
	}
	if (invert >= 0) {
		cfb_invert_area(display, 0, invert * FONT_H, UI_COLS * FONT_W, FONT_H);
	}
	cfb_framebuffer_finalize(display);
}

int hw_init(void)
{
	int err = 0;

	err |= gpio_pin_configure_dt(&haptic_pin, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&vbat_en, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&charge_fast, GPIO_OUTPUT_ACTIVE);      /* 100 mA charge */
	err |= adc_channel_setup_dt(&vbat_adc);
	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return -ENODEV;
	}
	err |= display_blanking_off(display);
	err |= cfb_framebuffer_init(display);
	for (int i = 0; i < cfb_get_numof_fonts(display); i++) {
		uint8_t w, h;

		cfb_get_font_size(display, i, &w, &h);
		if (w == FONT_W && h == FONT_H) {
			cfb_framebuffer_set_font(display, i);
		}
	}
	return err ? -EIO : 0;
}
