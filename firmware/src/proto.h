/*
 * Pager wire protocol v1. See docs/protocol.md.
 * Pure C, no Zephyr dependencies: this file is also compiled and tested on the host.
 */
#ifndef PAGER_PROTO_H
#define PAGER_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_VERSION    1
#define PROTO_MAX_FRAME  180
#define PROTO_MAX_STR    160
#define PROTO_MAX_NUMBER 20
#define PROTO_MAX_NAME   32

enum proto_type {
	/* pager -> cloud */
	PT_HELLO = 0x01,
	PT_SYNC = 0x02,
	PT_REPLY = 0x03,
	PT_VM_GET = 0x04,
	PT_VM_MARK = 0x05,
	/* cloud -> pager */
	PT_TIME = 0x81,
	PT_CANNED = 0x82,
	PT_EV_CALL = 0x83,
	PT_EV_VM = 0x84,
	PT_VM_TEXT = 0x85,
	PT_REPLY_RESULT = 0x86,
	PT_SYNC_DONE = 0x87,
};

enum proto_link { LINK_BLE_BRIDGE = 0, LINK_LTE = 1 };
enum proto_vm_action { VM_HEARD = 1, VM_DELETE = 2 };
enum proto_reply_status {
	REPLY_SENT = 0,
	REPLY_QUEUED = 1,     /* will go to the next caller that reaches voicemail within 2 min */
	REPLY_NO_CALLER = 2,
	REPLY_FAILED = 3,
};

struct proto_msg {
	uint8_t type;
	uint8_t seq;
	union {
		struct {
			uint8_t proto, fw_major, fw_minor, batt_pct, link;
			uint16_t cfg_ver;
			uint32_t last_event;
		} hello;
		struct { uint32_t last_event; } sync;
		struct { uint32_t call_event; uint8_t canned_idx; } reply;
		struct { uint32_t vm_id; uint16_t offset; } vm_get;
		struct { uint32_t vm_id; uint8_t action; } vm_mark;

		struct { uint32_t unix_time; int16_t tz_min; } time;
		struct {
			uint16_t cfg_ver;
			uint8_t idx, count;
			char text[PROTO_MAX_STR + 1];
		} canned;
		struct {
			uint32_t event, ts;
			uint8_t flags;
			char number[PROTO_MAX_NUMBER + 1];
			char name[PROTO_MAX_NAME + 1];
		} ev_call;
		struct {
			uint32_t event, vm_id, ts;
			uint16_t duration_s, text_len;
			char number[PROTO_MAX_NUMBER + 1];
			char name[PROTO_MAX_NAME + 1];
		} ev_vm;
		struct {
			uint32_t vm_id;
			uint16_t offset, total;
			char chunk[PROTO_MAX_STR + 1];
		} vm_text;
		struct { uint32_t call_event; uint8_t status; } reply_result;
		struct { uint32_t last_event; uint8_t unheard; } sync_done;
	} u;
};

/* Returns the frame length, or a negative value if the message cannot be encoded. */
int proto_encode(const struct proto_msg *m, uint8_t *buf, size_t cap);

/* Returns 0 on success, negative on a malformed, truncated or unknown frame. */
int proto_decode(const uint8_t *buf, size_t len, struct proto_msg *m);

/* Canonical one-line text form, shared with the backend's test vectors. */
int proto_dump(const struct proto_msg *m, char *out, size_t cap);

#endif
