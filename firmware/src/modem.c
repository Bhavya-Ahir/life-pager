/*
 * LTE-M away link: an nRF9151 running Nordic's Serial Modem application, driven with AT
 * commands over uart0. MQTT runs inside the modem; payloads are protocol frames as hex text
 * (AT command lines are text, and the frames are tiny, so hex is the robust choice).
 *
 * The AT#X... commands below follow the Serial LTE Modem / Serial Modem MQTT client reference.
 * VERIFY them against the AT command reference of the Serial Modem version you flash - Nordic
 * has renamed parts of this interface between releases. Everything modem-specific is in this
 * one file for that reason.
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_DECLARE(pager);

#define USER DT_PATH(zephyr_user)
#define LINE_MAX 420            /* a 180-byte frame as hex plus slack */
#define TOPIC_UP "pager/" CONFIG_PAGER_ID "/up"
#define TOPIC_DOWN "pager/" CONFIG_PAGER_ID "/down"
#define EDRX_LTE_M_20S "0010"   /* 20.48 s eDRX cycle: worst-case downlink latency */
#define KEEPALIVE_S 900

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct gpio_dt_spec pin_en = GPIO_DT_SPEC_GET(USER, modem_en_gpios);
static const struct gpio_dt_spec pin_dtr = GPIO_DT_SPEC_GET(USER, modem_dtr_gpios);

K_MSGQ_DEFINE(line_q, LINE_MAX, 4, 1);
static K_SEM_DEFINE(wake, 0, 1);
static K_MUTEX_DEFINE(tx_lock);
static atomic_t want_on;
static bool connected;

/* ------------------------------------------------------------------ UART plumbing */
static void uart_isr(const struct device *dev, void *user)
{
	static char line[LINE_MAX];
	static size_t n;
	uint8_t c;

	ARG_UNUSED(user);
	if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
		return;
	}
	while (uart_fifo_read(dev, &c, 1) == 1) {
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			if (n > 0) {
				line[n] = 0;
				(void)k_msgq_put(&line_q, line, K_NO_WAIT);
				n = 0;
			}
		} else if (n < LINE_MAX - 1) {
			line[n++] = (char)c;
		}
	}
}

static void at_write(const char *s)
{
	for (; *s; s++) {
		uart_poll_out(uart, *s);
	}
	uart_poll_out(uart, '\r');
	uart_poll_out(uart, '\n');
}

static int from_hex(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = strlen(hex);

	if (n % 2 || n / 2 > cap) {
		return -1;
	}
	for (size_t i = 0; i < n / 2; i++) {
		unsigned int v;

		if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
			return -1;
		}
		out[i] = (uint8_t)v;
	}
	return (int)(n / 2);
}

static void set_connected(bool up)
{
	struct app_evt e = { .type = EVT_LTE, .arg = up };

	if (connected != up) {
		connected = up;
		app_post(&e);
	}
}

/* Unsolicited lines. Returns true if the line was one. */
static bool handle_urc(const char *line)
{
	static uint8_t expect;          /* after #XMQTTMSG: 1 = topic line next, 2 = payload next */
	int a, b;

	if (expect == 1) {
		expect = 2;
		return true;
	}
	if (expect == 2) {
		struct app_evt e = { .type = EVT_FRAME };
		int len = from_hex(line, e.data, sizeof(e.data));

		expect = 0;
		if (len >= 2) {
			e.len = (uint8_t)len;
			app_post(&e);
		}
		return true;
	}
	if (strncmp(line, "#XMQTTMSG:", 10) == 0) {
		expect = 1;
		return true;
	}
	if (sscanf(line, "#XMQTTEVT: %d,%d", &a, &b) == 2) {
		if (a == 0 && b == 0) {
			set_connected(true);            /* CONNACK */
		} else if (a == 1 || (a == 0 && b != 0)) {
			set_connected(false);           /* disconnected or refused */
		}
		return true;
	}
	return line[0] == '+' || line[0] == '%' || line[0] == '#';
}

/* Send a command and wait for OK / ERROR. `expect`, if given, must be seen before OK. */
static int at_cmd(const char *cmd, const char *expect, int timeout_s)
{
	char line[LINE_MAX];
	int64_t deadline = k_uptime_get() + timeout_s * 1000LL;
	bool seen = expect == NULL;

	k_mutex_lock(&tx_lock, K_FOREVER);
	at_write(cmd);
	k_mutex_unlock(&tx_lock);
	while (k_uptime_get() < deadline) {
		if (k_msgq_get(&line_q, line, K_MSEC(200)) != 0) {
			continue;
		}
		if (expect && strstr(line, expect)) {
			seen = true;
		}
		if (strcmp(line, "OK") == 0) {
			return seen ? 0 : -ENOMSG;
		}
		if (strstr(line, "ERROR")) {
			return -EIO;
		}
		(void)handle_urc(line);
	}
	return -ETIMEDOUT;
}

static int wait_for(const char *a, const char *b, int timeout_s)
{
	char line[LINE_MAX];
	int64_t deadline = k_uptime_get() + timeout_s * 1000LL;

	while (k_uptime_get() < deadline && atomic_get(&want_on)) {
		if (k_msgq_get(&line_q, line, K_MSEC(500)) != 0) {
			continue;
		}
		if (strstr(line, a) || (b && strstr(line, b))) {
			return 0;
		}
		(void)handle_urc(line);
	}
	return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ power + session */
static void power(bool on)
{
	if (on) {
		gpio_pin_configure_dt(&pin_dtr, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set_dt(&pin_en, 1);
		uart_irq_rx_enable(uart);
	} else {
		uart_irq_rx_disable(uart);
		/* Do not back-power the modem through its I/O pins while its supply is off */
		gpio_pin_configure_dt(&pin_dtr, GPIO_DISCONNECTED);
		gpio_pin_set_dt(&pin_en, 0);
	}
}

static int bring_up(void)
{
	char cmd[200];

	power(true);
	(void)wait_for("Ready", NULL, 8);
	if (at_cmd("AT", NULL, 3) != 0) {
		return -ENODEV;
	}
	(void)at_cmd("AT+CEREG=5", NULL, 3);
	(void)at_cmd("AT%XSYSTEMMODE=1,0,0,0", NULL, 3);                 /* LTE-M only */
	(void)at_cmd("AT+CEDRXS=2,4,\"" EDRX_LTE_M_20S "\"", NULL, 3);
	if (at_cmd("AT+CFUN=1", NULL, 10) != 0) {
		return -EIO;
	}
	if (wait_for("+CEREG: 1", "+CEREG: 5", 180) != 0) {
		return -ENETUNREACH;
	}
	snprintf(cmd, sizeof(cmd), "AT#XMQTTCFG=\"%s\",%d,0", CONFIG_PAGER_ID, KEEPALIVE_S);
	(void)at_cmd(cmd, NULL, 3);
	snprintf(cmd, sizeof(cmd), "AT#XMQTTCON=1,\"%s\",\"%s\",\"%s\",%d,%d", CONFIG_PAGER_MQTT_USER,
		 CONFIG_PAGER_MQTT_PASS, CONFIG_PAGER_MQTT_HOST, CONFIG_PAGER_MQTT_PORT,
		 CONFIG_PAGER_MQTT_SEC_TAG);
	if (at_cmd(cmd, NULL, 60) != 0) {
		return -ECONNREFUSED;
	}
	for (int i = 0; i < 60 && !connected; i++) {
		char line[LINE_MAX];

		if (k_msgq_get(&line_q, line, K_MSEC(500)) == 0) {
			(void)handle_urc(line);
		}
	}
	if (!connected) {
		return -ETIMEDOUT;
	}
	return at_cmd("AT#XMQTTSUB=\"" TOPIC_DOWN "\",1", NULL, 20);
}

static void shut_down(void)
{
	if (connected) {
		(void)at_cmd("AT#XMQTTCON=0", NULL, 5);
	}
	(void)at_cmd("AT+CFUN=0", NULL, 5);
	power(false);
	set_connected(false);
}

static void modem_thread(void *a, void *b, void *c)
{
	char line[LINE_MAX];
	int backoff_s = 30;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&wake, K_FOREVER);
		while (atomic_get(&want_on)) {
			int err = bring_up();

			if (err) {
				LOG_WRN("LTE bring-up failed: %d, retry in %d s", err, backoff_s);
				shut_down();
				k_sem_take(&wake, K_SECONDS(backoff_s));
				backoff_s = MIN(backoff_s * 2, 1800);    /* do not burn the battery with no coverage */
				continue;
			}
			backoff_s = 30;
			while (atomic_get(&want_on) && connected) {
				if (k_msgq_get(&line_q, line, K_MSEC(500)) == 0) {
					(void)handle_urc(line);
				}
			}
			shut_down();
		}
	}
}
K_THREAD_DEFINE(modem_tid, 3072, modem_thread, NULL, NULL, NULL, 7, 0, 0);

/* ------------------------------------------------------------------ public */
void modem_request(bool on)
{
	if (!IS_ENABLED(CONFIG_PAGER_LTE)) {
		return;
	}
	if (atomic_set(&want_on, on) != on) {
		k_sem_give(&wake);
	}
}

bool modem_connected(void)
{
	return connected;
}

int modem_send(const uint8_t *frame, size_t len)
{
	char cmd[sizeof("AT#XMQTTPUB=\"" TOPIC_UP "\",\"\",1,0") + 2 * PROTO_MAX_FRAME + 1];
	int n;

	if (!connected) {
		return -ENOTCONN;
	}
	if (len > PROTO_MAX_FRAME) {
		return -EMSGSIZE;
	}
	n = snprintf(cmd, sizeof(cmd), "AT#XMQTTPUB=\"" TOPIC_UP "\",\"");
	for (size_t i = 0; i < len; i++) {
		n += snprintf(cmd + n, sizeof(cmd) - n, "%02x", frame[i]);
	}
	snprintf(cmd + n, sizeof(cmd) - n, "\",1,0");
	/* Fire and forget: the modem thread owns the response stream and consumes the OK */
	k_mutex_lock(&tx_lock, K_FOREVER);
	at_write(cmd);
	k_mutex_unlock(&tx_lock);
	return 0;
}

int modem_init(void)
{
	if (!IS_ENABLED(CONFIG_PAGER_LTE)) {
		return 0;
	}
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	gpio_pin_configure_dt(&pin_en, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&pin_dtr, GPIO_DISCONNECTED);
	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	return 0;
}
