/*
 * Pager application model: inbox, canned replies, and the whole UI state machine.
 * Pure C with no Zephyr dependencies. Input: buttons, ANCS call events, protocol frames, ticks.
 * Output: protocol frames to send, haptic patterns, ANCS actions, and a text grid to draw.
 * Compiled and tested on the host (firmware/tests).
 */
#ifndef PAGER_MODEL_H
#define PAGER_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "proto.h"

#define UI_COLS 21          /* 168 px / 8 px font */
#define UI_ROWS 9           /* 144 px / 16 px font */
#define INBOX_MAX 12
#define CANNED_MAX 8
#define VM_TEXT_MAX 640     /* longer transcripts are cut on the pager; the app has the rest */
#define FW_MAJOR 0
#define FW_MINOR 1

enum button { BTN_UP, BTN_SELECT, BTN_DOWN };
enum haptic { HAPTIC_TICK, HAPTIC_RING, HAPTIC_ALERT, HAPTIC_STOP };
enum screen { SCR_HOME, SCR_RINGING, SCR_INBOX, SCR_ITEM, SCR_REPLY, SCR_VMTEXT };

struct inbox_item {
	bool is_vm, heard, replied;
	uint32_t event, vm_id, ts;
	uint16_t duration_s, text_len;
	char number[PROTO_MAX_NUMBER + 1];
	char name[PROTO_MAX_NAME + 1];
};

struct pager_io {
	void (*send)(const struct proto_msg *m, void *ctx);   /* to cloud, over whichever link is up */
	void (*haptic)(enum haptic h, void *ctx);
	void (*decline_call)(void *ctx);                      /* ANCS negative action */
	void *ctx;
};

struct pager {
	struct pager_io io;
	enum screen screen;
	uint8_t seq;

	/* status */
	uint8_t batt_pct, link;
	bool link_up;
	uint32_t unix_at_sync, uptime_at_sync, now;
	int16_t tz_min;
	uint8_t unheard;

	/* config */
	uint16_t cfg_ver, cfg_ver_rx;
	uint8_t canned_count;
	char canned[CANNED_MAX][PROTO_MAX_STR + 1];

	/* inbox, newest first */
	struct inbox_item inbox[INBOX_MAX];
	uint8_t inbox_count;
	uint32_t last_event;

	/* ui */
	uint8_t cursor, top;
	uint8_t item;                 /* index into inbox for SCR_ITEM / SCR_VMTEXT */
	bool reply_live;              /* reply targets the call that is ringing (call_event 0) */
	char ring_who[PROTO_MAX_NAME + 1];
	uint32_t ring_seen_at;
	bool ringing;
	char toast[UI_COLS + 1];
	uint32_t toast_until;
	uint32_t last_input;

	/* voicemail reader */
	char vm_text[VM_TEXT_MAX + 1];
	uint16_t vm_have, vm_total;
	uint32_t vm_reading;
	bool dirty;
};

void pager_init(struct pager *p, const struct pager_io *io);

/* Link management tells the model when a path to the cloud appears or goes away. */
void pager_link_changed(struct pager *p, bool up, uint8_t link, uint32_t now);
void pager_set_battery(struct pager *p, uint8_t pct);

void pager_on_frame(struct pager *p, const struct proto_msg *m, uint32_t now);
void pager_on_button(struct pager *p, enum button b, uint32_t now);
void pager_on_ring(struct pager *p, const char *who, uint32_t now);   /* ANCS incoming call */
void pager_on_ring_end(struct pager *p, uint32_t now);
void pager_tick(struct pager *p, uint32_t now);                       /* call about once a second */

/* Fills rows with NUL-terminated lines of at most UI_COLS chars. Row `*invert` is highlighted
 * (or -1). */
void pager_render(const struct pager *p, char rows[UI_ROWS][UI_COLS + 1], int *invert);

#endif
