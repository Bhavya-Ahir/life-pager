/* Drives the pager model the way the firmware does and checks what the user would see. */
#include <stdio.h>
#include <string.h>

#include "model.h"

static struct proto_msg sent[32];
static int n_sent, n_declined, last_haptic = -1, n_alert;
static int fails;

static void on_send(const struct proto_msg *m, void *ctx) { (void)ctx; if (n_sent < 32) sent[n_sent++] = *m; }
static void on_haptic(enum haptic h, void *ctx) { (void)ctx; last_haptic = h; if (h == HAPTIC_ALERT) n_alert++; }
static void on_decline(void *ctx) { (void)ctx; n_declined++; }

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char rows[UI_ROWS][UI_COLS + 1];
static int inv;

static int screen_has(struct pager *p, const char *text)
{
	pager_render(p, rows, &inv);
	for (int r = 0; r < UI_ROWS; r++) {
		if (strlen(rows[r]) > UI_COLS) return 0;
		if (strstr(rows[r], text)) return 1;
	}
	return 0;
}

static void show(struct pager *p, const char *title)
{
	pager_render(p, rows, &inv);
	printf("+- %-19s-+\n", title);
	for (int r = 0; r < UI_ROWS; r++) printf("|%c%-21s|\n", r == inv ? '>' : ' ', rows[r]);
	printf("+----------------------+\n");
}

static struct proto_msg call(uint32_t ev, uint32_t ts, const char *num, const char *name)
{
	struct proto_msg m = { .type = PT_EV_CALL };

	m.u.ev_call.event = ev; m.u.ev_call.ts = ts;
	snprintf(m.u.ev_call.number, sizeof(m.u.ev_call.number), "%s", num);
	snprintf(m.u.ev_call.name, sizeof(m.u.ev_call.name), "%s", name);
	return m;
}

static struct proto_msg vm(uint32_t ev, uint32_t id, uint32_t ts, uint16_t len)
{
	struct proto_msg m = { .type = PT_EV_VM };

	m.u.ev_vm.event = ev; m.u.ev_vm.vm_id = id; m.u.ev_vm.ts = ts;
	m.u.ev_vm.duration_s = 37; m.u.ev_vm.text_len = len;
	snprintf(m.u.ev_vm.number, sizeof(m.u.ev_vm.number), "+14155550123");
	snprintf(m.u.ev_vm.name, sizeof(m.u.ev_vm.name), "Asha Patel");
	return m;
}

static void boot(struct pager *p, uint32_t unix_time)
{
	struct pager_io io = { on_send, on_haptic, on_decline, NULL };
	struct proto_msg m = { .type = PT_TIME };
	const char *canned[] = { "Away from my phone. Call you back in 10.", "Leave a voicemail if urgent.", "Zo\xc3\xab says hi" };

	n_sent = n_declined = n_alert = 0; last_haptic = -1;
	pager_init(p, &io);
	pager_link_changed(p, true, LINK_BLE_BRIDGE, 100);
	m.u.time.unix_time = unix_time; m.u.time.tz_min = -480;
	pager_on_frame(p, &m, 100);
	for (uint8_t i = 0; i < 3; i++) {
		struct proto_msg c = { .type = PT_CANNED };

		c.u.canned.cfg_ver = 5; c.u.canned.idx = i; c.u.canned.count = 3;
		snprintf(c.u.canned.text, sizeof(c.u.canned.text), "%s", canned[i]);
		pager_on_frame(p, &c, 100);
	}
}

int main(void)
{
	static struct pager p;
	const uint32_t T0 = 1794700800;   /* 2026-11-15 00:00:00 UTC -> 16:00 at UTC-8 */
	struct proto_msg m;

	/* ---- boot: hello goes out, time and canned replies come in */
	boot(&p, T0);
	CHECK(n_sent == 1 && sent[0].type == PT_HELLO && sent[0].u.hello.cfg_ver == 0);
	CHECK(p.cfg_ver == 5 && p.canned_count == 3);
	CHECK(strcmp(p.canned[2], "Zo? says hi") == 0);
	CHECK(screen_has(&p, "BT 100%") && screen_has(&p, "16:00"));
	show(&p, "home");

	/* ---- near the phone: it rings, decline + reply to the live call */
	pager_on_ring(&p, "Asha Patel", 110);
	CHECK(p.screen == SCR_RINGING && last_haptic == HAPTIC_RING && screen_has(&p, "Asha Patel"));
	show(&p, "ringing");
	pager_on_button(&p, BTN_SELECT, 112);
	CHECK(n_declined == 1 && p.screen == SCR_REPLY && last_haptic == HAPTIC_STOP);
	show(&p, "reply picker");
	pager_on_button(&p, BTN_DOWN, 113);
	n_sent = 0;
	pager_on_button(&p, BTN_SELECT, 114);
	CHECK(n_sent == 1 && sent[0].type == PT_REPLY && sent[0].u.reply.call_event == 0 && sent[0].u.reply.canned_idx == 1);
	CHECK(p.screen == SCR_HOME && screen_has(&p, "Sending..."));
	pager_on_ring_end(&p, 115);
	m = (struct proto_msg){ .type = PT_REPLY_RESULT }; m.u.reply_result.status = REPLY_QUEUED;
	pager_on_frame(&p, &m, 116);
	CHECK(screen_has(&p, "Reply queued"));
	/* the forwarded call shows up; we already buzzed for it, so no second alert */
	m = call(1, T0 + 16, "+14155550123", "Asha Patel");
	pager_on_frame(&p, &m, 118);
	CHECK(n_alert == 0 && p.inbox_count == 1 && p.screen == SCR_HOME);
	/* resync is scheduled after the ring ended */
	n_sent = 0;
	for (uint32_t t = 119; t < 140; t++) pager_tick(&p, t);
	CHECK(n_sent == 1 && sent[0].type == PT_SYNC && sent[0].u.sync.last_event == 1);
	CHECK(!screen_has(&p, "Reply queued"));                  /* toast expired */

	/* ---- silence leaves the call alone */
	pager_on_ring(&p, "+14155550000", 200);
	pager_on_button(&p, BTN_UP, 201);
	CHECK(n_declined == 1 && p.screen == SCR_RINGING && last_haptic == HAPTIC_STOP);
	pager_on_ring_end(&p, 220);
	CHECK(p.screen == SCR_HOME);

	/* ---- away from the phone: a fresh call arrives over LTE and buzzes */
	boot(&p, T0);
	pager_link_changed(&p, true, LINK_LTE, 100);
	CHECK(screen_has(&p, "LTE 100%"));
	m = call(7, T0 + 400, "+14155550123", "");
	pager_on_frame(&p, &m, 500);
	CHECK(n_alert == 1 && p.screen == SCR_ITEM && screen_has(&p, "+14155550123") && screen_has(&p, "Missed call"));
	show(&p, "away: missed call");
	n_sent = 0;
	pager_on_button(&p, BTN_SELECT, 501);                    /* Reply by text */
	CHECK(p.screen == SCR_REPLY);
	pager_on_button(&p, BTN_SELECT, 502);
	CHECK(n_sent == 1 && sent[0].type == PT_REPLY && sent[0].u.reply.call_event == 7 && sent[0].u.reply.canned_idx == 0);
	m = (struct proto_msg){ .type = PT_REPLY_RESULT }; m.u.reply_result.call_event = 7;
	pager_on_frame(&p, &m, 503);
	CHECK(p.inbox[0].replied && screen_has(&p, "Reply sent"));

	/* ---- voicemail: recording first (no text), then the transcript updates the same entry */
	m = vm(8, 3, T0 + 400, 0);
	pager_on_frame(&p, &m, 520);
	CHECK(n_alert == 2 && p.inbox_count == 2 && p.inbox[0].is_vm);
	m = vm(9, 3, T0 + 400, 200);
	pager_on_frame(&p, &m, 540);
	CHECK(p.inbox_count == 2 && p.inbox[0].text_len == 200 && p.last_event == 9 && n_alert == 2);
	m = vm(9, 3, T0 + 400, 999);                             /* duplicate delivery is ignored */
	pager_on_frame(&p, &m, 541);
	CHECK(p.inbox[0].text_len == 200);
	m = (struct proto_msg){ .type = PT_SYNC_DONE }; m.u.sync_done.unheard = 1; m.u.sync_done.last_event = 9;
	n_sent = 0;
	pager_on_frame(&p, &m, 542);
	CHECK(n_sent == 1 && sent[0].type == PT_SYNC && sent[0].u.sync.last_event == 9);   /* drains backlog */
	pager_on_frame(&p, &m, 543);
	CHECK(n_sent == 1);                                      /* nothing new: stops asking */

	for (uint32_t t = 544; t < 570; t++) pager_tick(&p, t);
	CHECK(p.screen == SCR_HOME && screen_has(&p, "1 new voicemail"));
	show(&p, "home with voicemail");
	pager_on_button(&p, BTN_SELECT, 570);
	CHECK(p.screen == SCR_INBOX && screen_has(&p, "VAsha Patel"));
	show(&p, "inbox");
	pager_on_button(&p, BTN_SELECT, 571);
	CHECK(p.screen == SCR_ITEM && screen_has(&p, "Read message") && screen_has(&p, "Voicemail 37s"));
	n_sent = 0;
	pager_on_button(&p, BTN_SELECT, 572);                    /* Read message */
	CHECK(p.screen == SCR_VMTEXT && n_sent == 1 && sent[0].type == PT_VM_GET && sent[0].u.vm_get.offset == 0);
	CHECK(screen_has(&p, "Loading..."));
	m = (struct proto_msg){ .type = PT_VM_TEXT };
	m.u.vm_text.vm_id = 3; m.u.vm_text.total = 200;
	memset(m.u.vm_text.chunk, 0, sizeof(m.u.vm_text.chunk));
	snprintf(m.u.vm_text.chunk, sizeof(m.u.vm_text.chunk), "%s",
		 "Hey it is Asha, I wanted to check whether you are still coming on Saturday. "
		 "Call me back when you get a chance, no rush, okay? Bye for now.");
	for (size_t l = strlen(m.u.vm_text.chunk); l < 160; l++) {
		m.u.vm_text.chunk[l] = '.';
		m.u.vm_text.chunk[l + 1] = 0;
	}
	pager_on_frame(&p, &m, 573);
	CHECK(n_sent == 2 && sent[1].type == PT_VM_GET && sent[1].u.vm_get.offset == 160);
	m.u.vm_text.offset = 0;                                  /* stale duplicate chunk is ignored */
	pager_on_frame(&p, &m, 573);
	CHECK(p.vm_have == 160 && n_sent == 2);
	m.u.vm_text.offset = 160;
	strcpy(m.u.vm_text.chunk, " This is the last forty bytes of text!!!");
	pager_on_frame(&p, &m, 574);
	CHECK(p.vm_have == 200 && n_sent == 3 && sent[2].type == PT_VM_MARK && sent[2].u.vm_mark.action == VM_HEARD);
	CHECK(p.inbox[0].heard && screen_has(&p, "Hey it is Asha, I"));
	show(&p, "voicemail text");
	pager_on_button(&p, BTN_DOWN, 575);
	CHECK(!screen_has(&p, "Hey it is Asha, I") && screen_has(&p, "Call me back"));
	pager_on_button(&p, BTN_SELECT, 576);
	CHECK(p.screen == SCR_ITEM && p.item == 0);

	/* delete it: cursor to Delete (Read, Reply, Delete, Back) */
	pager_on_button(&p, BTN_DOWN, 577);
	pager_on_button(&p, BTN_DOWN, 578);
	show(&p, "item menu");
	n_sent = 0;
	pager_on_button(&p, BTN_SELECT, 579);
	CHECK(n_sent == 1 && sent[0].type == PT_VM_MARK && sent[0].u.vm_mark.action == VM_DELETE && sent[0].u.vm_mark.vm_id == 3);
	CHECK(p.inbox_count == 1 && !p.inbox[0].is_vm && p.screen == SCR_INBOX);

	/* ---- offline: actions that need the cloud say so instead of pretending */
	pager_link_changed(&p, false, 0, 600);
	CHECK(screen_has(&p, "-- 100%"));
	p.inbox[0].replied = false;
	pager_on_button(&p, BTN_SELECT, 601);                    /* open item */
	pager_on_button(&p, BTN_SELECT, 602);                    /* Reply by text */
	n_sent = 0;
	pager_on_button(&p, BTN_SELECT, 603);
	CHECK(n_sent == 0 && screen_has(&p, "No connection"));

	/* ---- backlog on reconnect does not buzz; inbox overflow keeps the newest */
	boot(&p, T0 + 100000);
	for (uint32_t e = 1; e <= 20; e++) {
		m = call(e, T0 + e, "+14155550123", "");
		pager_on_frame(&p, &m, 700);
	}
	CHECK(n_alert == 0 && p.inbox_count == INBOX_MAX && p.inbox[0].event == 20 && p.last_event == 20);
	pager_on_button(&p, BTN_SELECT, 701);
	for (int i = 0; i < 30; i++) pager_on_button(&p, BTN_DOWN, 702);
	show(&p, "inbox scrolled");
	CHECK(inv >= 1 && inv < UI_ROWS);
	for (int i = 0; i < 13; i++) pager_on_button(&p, BTN_UP, 703);

	/* ---- malformed config rows cannot overflow the canned table */
	m = (struct proto_msg){ .type = PT_CANNED };
	m.u.canned.idx = 200; m.u.canned.count = 201; m.u.canned.cfg_ver = 9;
	pager_on_frame(&p, &m, 704);
	CHECK(p.canned_count == 3 && p.cfg_ver == 5);

	printf("model: %d failures\n", fails);
	return fails ? 1 : 0;
}
