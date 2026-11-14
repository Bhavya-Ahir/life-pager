/*
 * Bluetooth: one bonded iPhone.
 *
 *  1. ANCS client - the iPhone itself tells us about incoming calls (no app needed), and lets
 *     us decline them. Follows the nRF Connect SDK peripheral_ancs_client sample.
 *  2. Pager bridge service - the companion app relays opaque protocol frames between this
 *     service and the backend. Both characteristics require an encrypted (bonded) link.
 *
 *     service 7a1e0001-5b0c-4c1e-9f0a-6c1f5e9a2b10
 *     RX      7a1e0002-...  write / write without response   app -> pager, one frame per write
 *     TX      7a1e0003-...  notify                           pager -> app, one frame per notify
 */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/ancs_client.h>

#include "app.h"

LOG_MODULE_DECLARE(pager);

#define BRIDGE_UUID(n) BT_UUID_128_ENCODE(0x7a1e0000 + (n), 0x5b0c, 0x4c1e, 0x9f0a, 0x6c1f5e9a2b10)
static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(BRIDGE_UUID(1));
static const struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(BRIDGE_UUID(2));
static const struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(BRIDGE_UUID(3));

static struct bt_conn *conn;
static struct bt_ancs_client ancs;
static bool bridge_on, ancs_ready;
static uint32_t ring_uid;
static bool ring_active;
static uint8_t title_buf[PROTO_MAX_NAME + 1];

/* ------------------------------------------------------------------ bridge service */
static ssize_t rx_write(struct bt_conn *c, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	struct app_evt e = { .type = EVT_FRAME };

	ARG_UNUSED(c);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0 || len < 2 || len > PROTO_MAX_FRAME) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	e.len = (uint8_t)len;
	memcpy(e.data, buf, len);
	app_post(&e);
	return len;
}

static void tx_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	struct app_evt e = { .type = EVT_BLE_BRIDGE, .arg = (value == BT_GATT_CCC_NOTIFY) };

	ARG_UNUSED(attr);
	bridge_on = e.arg;
	app_post(&e);
}

BT_GATT_SERVICE_DEFINE(bridge_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, rx_write, NULL),
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(tx_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
);

bool ble_bridge_ready(void)
{
	return conn != NULL && bridge_on;
}

int ble_send(const uint8_t *frame, size_t len)
{
	if (!ble_bridge_ready()) {
		return -ENOTCONN;
	}
	if (len > bt_gatt_get_mtu(conn) - 3) {
		return -EMSGSIZE;
	}
	return bt_gatt_notify(conn, &bridge_svc.attrs[4], frame, len);
}

/* ------------------------------------------------------------------ ANCS */
static void ring_end(void)
{
	struct app_evt e = { .type = EVT_RING_END };

	if (ring_active) {
		ring_active = false;
		app_post(&e);
	}
}

static void ancs_notif_cb(struct bt_ancs_client *c, int err, const struct bt_ancs_evt_notif *n)
{
	if (err || n->category_id != BT_ANCS_CATEGORY_ID_INCOMING_CALL) {
		return;                               /* calls only - everything else is ignored */
	}
	if (n->evt_id == BT_ANCS_EVENT_ID_NOTIFICATION_ADDED && !n->evt_flags.pre_existing) {
		ring_uid = n->notif_uid;
		ring_active = true;
		/* Ask for the title = caller name or number; the ring is announced when it arrives */
		if (bt_ancs_request_attrs(c, n, NULL) != 0) {
			struct app_evt e = { .type = EVT_RING };

			app_post(&e);
		}
	} else if (n->evt_id == BT_ANCS_EVENT_ID_NOTIFICATION_REMOVED && n->notif_uid == ring_uid) {
		ring_end();
	}
}

static void ancs_data_cb(struct bt_ancs_client *c, const struct bt_ancs_attr_response *r)
{
	struct app_evt e = { .type = EVT_RING };

	ARG_UNUSED(c);
	if (r->command_id != BT_ANCS_COMMAND_ID_GET_NOTIF_ATTRIBUTES || r->notif_uid != ring_uid ||
	    r->attr.attr_id != BT_ANCS_NOTIF_ATTR_ID_TITLE || !ring_active) {
		return;
	}
	snprintf(e.text, sizeof(e.text), "%.*s", (int)MIN(r->attr.attr_len, PROTO_MAX_NAME),
		 (const char *)r->attr.attr_data);
	app_post(&e);
}

void ble_decline_call(void)
{
	if (ancs_ready && ring_active) {
		bt_ancs_notification_action(&ancs, ring_uid, BT_ANCS_ACTION_ID_NEGATIVE, NULL);
	}
}

static void dm_completed(struct bt_gatt_dm *dm, void *ctx)
{
	ARG_UNUSED(ctx);
	if (bt_ancs_handles_assign(dm, &ancs) == 0 &&
	    bt_ancs_subscribe_notification_source(&ancs, ancs_notif_cb) == 0 &&
	    bt_ancs_subscribe_data_source(&ancs, ancs_data_cb) == 0) {
		ancs_ready = true;
		LOG_INF("ANCS ready");
	}
	bt_gatt_dm_data_release(dm);
}

static void dm_not_found(struct bt_conn *c, void *ctx)
{
	ARG_UNUSED(c);
	ARG_UNUSED(ctx);
	LOG_WRN("peer has no ANCS (not an iPhone, or not bonded yet)");
}

static void dm_error(struct bt_conn *c, int err, void *ctx)
{
	ARG_UNUSED(c);
	ARG_UNUSED(ctx);
	LOG_WRN("ANCS discovery failed: %d", err);
}

static const struct bt_gatt_dm_cb dm_cb = {
	.completed = dm_completed,
	.service_not_found = dm_not_found,
	.error_found = dm_error,
};

/* ------------------------------------------------------------------ connection */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_SOLICIT128, BT_UUID_ANCS_VAL),
};
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BRIDGE_UUID(1)),
};

static void advertise(struct k_work *work)
{
	/* 1 s interval: slow enough to cost ~15 uA, fast enough that iOS reconnects in seconds */
	static const struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN, 0x0640, 0x0640, NULL);
	int err;

	ARG_UNUSED(work);
	err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err && err != -EALREADY) {
		LOG_WRN("advertising failed: %d", err);
	}
}
static K_WORK_DEFINE(adv_work, advertise);

static void connected(struct bt_conn *c, uint8_t err)
{
	if (err) {
		k_work_submit(&adv_work);
		return;
	}
	conn = bt_conn_ref(c);
	if (bt_conn_set_security(c, BT_SECURITY_L2) != 0) {
		LOG_WRN("could not request encryption");
	}
}

static void disconnected(struct bt_conn *c, uint8_t reason)
{
	struct app_evt e = { .type = EVT_BLE_BRIDGE, .arg = 0 };

	ARG_UNUSED(c);
	ARG_UNUSED(reason);
	if (conn) {
		bt_conn_unref(conn);
		conn = NULL;
	}
	ancs_ready = false;
	bridge_on = false;
	ring_end();
	app_post(&e);
	k_work_submit(&adv_work);
}

static void security_changed(struct bt_conn *c, bt_security_t level, enum bt_security_err err)
{
	if (err || level < BT_SECURITY_L2) {
		LOG_WRN("security failed: %d", err);
		return;
	}
	if (bt_gatt_dm_start(c, BT_UUID_ANCS, &dm_cb, NULL) != 0) {
		LOG_WRN("could not start ANCS discovery");
	}
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

int ble_init(void)
{
	int err = bt_enable(NULL);

	if (err) {
		return err;
	}
	settings_load();
	err = bt_ancs_client_init(&ancs);
	if (err) {
		return err;
	}
	err = bt_ancs_register_attr(&ancs, BT_ANCS_NOTIF_ATTR_ID_TITLE, title_buf, sizeof(title_buf) - 1);
	if (err) {
		return err;
	}
	k_work_submit(&adv_work);
	return 0;
}
