#include "model.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define IDLE_TIMEOUT_S 20
#define FRESH_EVENT_S 180       /* only buzz for events that just happened, not for backlog */
#define RING_MEMORY_S 90        /* a forwarded call this soon after a ring is the same call */
#define TOAST_S 3
#define RESYNC_FIRST_S 20       /* after a ring ends, look for the forwarded call / voicemail */
#define RESYNC_NEXT_S 60
#define RESYNC_COUNT 3

static uint32_t resync_at;
static uint8_t resync_left;
static bool got_events;

/* ------------------------------------------------------------------ helpers */
static void send(struct pager *p, struct proto_msg *m)
{
	m->seq = ++p->seq;
	if (p->io.send) p->io.send(m, p->io.ctx);
}

static void haptic(struct pager *p, enum haptic h)
{
	if (p->io.haptic) p->io.haptic(h, p->io.ctx);
}

static void toast(struct pager *p, const char *text, uint32_t now)
{
	snprintf(p->toast, sizeof(p->toast), "%s", text);
	p->toast_until = now + TOAST_S;
	p->dirty = true;
}

static void go(struct pager *p, enum screen s)
{
	p->screen = s;
	p->cursor = 0;
	p->top = 0;
	p->dirty = true;
}

static uint32_t unix_now(const struct pager *p)
{
	return p->unix_at_sync ? p->unix_at_sync + (p->now - p->uptime_at_sync) : 0;
}

/* The display font is ASCII. Collapse each non-ASCII code point to '?'. */
static void ascii_copy(char *dst, size_t cap, const char *src)
{
	size_t n = 0;

	for (const unsigned char *s = (const unsigned char *)src; *s && n + 1 < cap; s++) {
		if (*s < 0x80) {
			dst[n++] = (*s < 0x20) ? ' ' : (char)*s;
		} else if ((*s & 0xc0) != 0x80) {
			dst[n++] = '?';
		}
	}
	dst[n] = 0;
}

static const char *who(const struct inbox_item *it)
{
	if (it->name[0]) return it->name;
	if (it->number[0]) return it->number;
	return "Unknown caller";
}

static void send_sync(struct pager *p)
{
	struct proto_msg m = { .type = PT_SYNC };

	m.u.sync.last_event = p->last_event;
	got_events = false;
	send(p, &m);
}

/* ------------------------------------------------------------------ inbox */
static struct inbox_item *inbox_insert(struct pager *p)
{
	uint8_t n = p->inbox_count < INBOX_MAX ? p->inbox_count : INBOX_MAX - 1;

	memmove(&p->inbox[1], &p->inbox[0], n * sizeof(p->inbox[0]));
	memset(&p->inbox[0], 0, sizeof(p->inbox[0]));
	p->inbox_count = n + 1;
	if (p->screen == SCR_ITEM || p->screen == SCR_VMTEXT || p->screen == SCR_REPLY) {
		if (p->item + 1 < INBOX_MAX) {
			p->item++;                       /* keep pointing at the same entry */
		} else {
			go(p, SCR_HOME);                 /* it just fell off the end */
		}
	}
	return &p->inbox[0];
}

static void inbox_remove(struct pager *p, uint8_t idx)
{
	if (idx >= p->inbox_count) return;
	memmove(&p->inbox[idx], &p->inbox[idx + 1], (p->inbox_count - idx - 1) * sizeof(p->inbox[0]));
	p->inbox_count--;
}

static void alert_if_fresh(struct pager *p, uint32_t ts, uint32_t now, bool is_vm)
{
	uint32_t t = unix_now(p);
	bool fresh = t != 0 && ts + FRESH_EVENT_S >= t;
	bool same_call = p->ring_seen_at && now - p->ring_seen_at < RING_MEMORY_S;

	if (!fresh || p->ringing) return;
	if (!is_vm && same_call) return;       /* already buzzed for this call while it rang */
	haptic(p, HAPTIC_ALERT);
	if (p->screen == SCR_HOME || p->screen == SCR_INBOX) {
		go(p, SCR_ITEM);
		p->item = 0;
		p->reply_live = false;
		p->last_input = now;
	}
}

/* ------------------------------------------------------------------ item menu */
enum item_action { ACT_REPLY, ACT_READ, ACT_DELETE, ACT_BACK };

static uint8_t item_actions(const struct inbox_item *it, enum item_action out[4])
{
	uint8_t n = 0;

	if (it->is_vm && it->text_len > 0) out[n++] = ACT_READ;
	if (it->number[0] && !it->replied) out[n++] = ACT_REPLY;
	if (it->is_vm) out[n++] = ACT_DELETE;
	out[n++] = ACT_BACK;
	return n;
}

static void vm_request(struct pager *p)
{
	struct proto_msg m = { .type = PT_VM_GET };

	m.u.vm_get.vm_id = p->vm_reading;
	m.u.vm_get.offset = p->vm_have;
	send(p, &m);
}

static void vm_mark(struct pager *p, uint32_t vm_id, uint8_t action)
{
	struct proto_msg m = { .type = PT_VM_MARK };

	m.u.vm_mark.vm_id = vm_id;
	m.u.vm_mark.action = action;
	send(p, &m);
}

/* ------------------------------------------------------------------ public */
void pager_init(struct pager *p, const struct pager_io *io)
{
	memset(p, 0, sizeof(*p));
	p->io = *io;
	p->batt_pct = 100;
	p->dirty = true;
	resync_at = 0;
	resync_left = 0;
	got_events = false;
}

void pager_set_battery(struct pager *p, uint8_t pct)
{
	if (pct != p->batt_pct) p->dirty = true;
	p->batt_pct = pct;
}

void pager_link_changed(struct pager *p, bool up, uint8_t link, uint32_t now)
{
	p->now = now;
	p->link_up = up;
	p->link = link;
	p->dirty = true;
	if (up) {
		struct proto_msg m = { .type = PT_HELLO };

		m.u.hello.proto = PROTO_VERSION;
		m.u.hello.fw_major = FW_MAJOR;
		m.u.hello.fw_minor = FW_MINOR;
		m.u.hello.batt_pct = p->batt_pct;
		m.u.hello.link = link;
		m.u.hello.cfg_ver = p->cfg_ver;
		m.u.hello.last_event = p->last_event;
		got_events = false;
		send(p, &m);
	}
}

void pager_on_frame(struct pager *p, const struct proto_msg *m, uint32_t now)
{
	struct inbox_item *it = NULL;

	p->now = now;
	p->dirty = true;
	switch (m->type) {
	case PT_TIME:
		p->unix_at_sync = m->u.time.unix_time;
		p->uptime_at_sync = now;
		p->tz_min = m->u.time.tz_min;
		break;
	case PT_CANNED:
		if (m->u.canned.idx >= CANNED_MAX || m->u.canned.count > CANNED_MAX ||
		    m->u.canned.idx >= m->u.canned.count) {
			break;
		}
		if (m->u.canned.cfg_ver != p->cfg_ver_rx) {      /* first row of a new version */
			p->cfg_ver_rx = m->u.canned.cfg_ver;
			p->cfg_ver = 0;                      /* not complete yet */
		}
		ascii_copy(p->canned[m->u.canned.idx], sizeof(p->canned[0]), m->u.canned.text);
		if (m->u.canned.idx == m->u.canned.count - 1) {
			p->canned_count = m->u.canned.count;
			p->cfg_ver = m->u.canned.cfg_ver;
		}
		break;
	case PT_EV_CALL:
		if (m->u.ev_call.event <= p->last_event) break;      /* duplicate delivery */
		p->last_event = m->u.ev_call.event;
		got_events = true;
		it = inbox_insert(p);
		it->event = m->u.ev_call.event;
		it->ts = m->u.ev_call.ts;
		snprintf(it->number, sizeof(it->number), "%s", m->u.ev_call.number);
		ascii_copy(it->name, sizeof(it->name), m->u.ev_call.name);
		alert_if_fresh(p, it->ts, now, false);
		break;
	case PT_EV_VM:
		if (m->u.ev_vm.event <= p->last_event) break;
		p->last_event = m->u.ev_vm.event;
		got_events = true;
		for (uint8_t i = 0; i < p->inbox_count; i++) {
			if (p->inbox[i].is_vm && p->inbox[i].vm_id == m->u.ev_vm.vm_id) {
				it = &p->inbox[i];           /* transcript arrived for a known voicemail */
			}
		}
		if (it != NULL) {
			it->event = m->u.ev_vm.event;
			it->duration_s = m->u.ev_vm.duration_s;
			it->text_len = m->u.ev_vm.text_len;
			break;
		}
		it = inbox_insert(p);
		it->is_vm = true;
		it->event = m->u.ev_vm.event;
		it->vm_id = m->u.ev_vm.vm_id;
		it->ts = m->u.ev_vm.ts;
		it->duration_s = m->u.ev_vm.duration_s;
		it->text_len = m->u.ev_vm.text_len;
		snprintf(it->number, sizeof(it->number), "%s", m->u.ev_vm.number);
		ascii_copy(it->name, sizeof(it->name), m->u.ev_vm.name);
		alert_if_fresh(p, it->ts, now, true);
		break;
	case PT_VM_TEXT: {
		size_t l = strlen(m->u.vm_text.chunk);
		bool done;

		if (m->u.vm_text.vm_id != p->vm_reading || m->u.vm_text.offset != p->vm_have) break;
		p->vm_total = m->u.vm_text.total;
		if (p->vm_have + l > VM_TEXT_MAX) l = VM_TEXT_MAX - p->vm_have;
		memcpy(p->vm_text + p->vm_have, m->u.vm_text.chunk, l);
		p->vm_have += l;
		p->vm_text[p->vm_have] = 0;
		done = l == 0 || p->vm_have >= p->vm_total || p->vm_have >= VM_TEXT_MAX;
		if (!done) {
			vm_request(p);
			break;
		}
		for (uint8_t i = 0; i < p->inbox_count; i++) {
			if (p->inbox[i].is_vm && p->inbox[i].vm_id == p->vm_reading && !p->inbox[i].heard) {
				p->inbox[i].heard = true;
				vm_mark(p, p->vm_reading, VM_HEARD);
			}
		}
		break;
	}
	case PT_REPLY_RESULT:
		switch (m->u.reply_result.status) {
		case REPLY_SENT: toast(p, "Reply sent", now); haptic(p, HAPTIC_TICK); break;
		case REPLY_QUEUED: toast(p, "Reply queued", now); break;
		case REPLY_NO_CALLER: toast(p, "No number to text", now); break;
		default: toast(p, "Reply failed", now); break;
		}
		if (m->u.reply_result.status == REPLY_SENT) {
			for (uint8_t i = 0; i < p->inbox_count; i++) {
				if (p->inbox[i].event == m->u.reply_result.call_event) p->inbox[i].replied = true;
			}
		}
		break;
	case PT_SYNC_DONE:
		p->unheard = m->u.sync_done.unheard;
		if (got_events && p->link_up) send_sync(p);      /* drain the backlog */
		break;
	default:
		break;
	}
}

void pager_on_ring(struct pager *p, const char *caller, uint32_t now)
{
	p->now = now;
	p->ringing = true;
	p->ring_seen_at = now;
	ascii_copy(p->ring_who, sizeof(p->ring_who), caller && caller[0] ? caller : "Unknown caller");
	go(p, SCR_RINGING);
	p->last_input = now;
	haptic(p, HAPTIC_RING);
}

void pager_on_ring_end(struct pager *p, uint32_t now)
{
	p->now = now;
	if (!p->ringing) return;
	p->ringing = false;
	p->ring_seen_at = now;
	haptic(p, HAPTIC_STOP);
	if (p->screen == SCR_RINGING) go(p, SCR_HOME);
	resync_at = now + RESYNC_FIRST_S;
	resync_left = RESYNC_COUNT;
	p->dirty = true;
}

static void send_reply(struct pager *p, uint8_t idx, uint32_t now)
{
	struct proto_msg m = { .type = PT_REPLY };

	if (!p->link_up) {
		toast(p, "No connection", now);
		return;
	}
	m.u.reply.call_event = p->reply_live ? 0 : p->inbox[p->item].event;
	m.u.reply.canned_idx = idx;
	send(p, &m);
	toast(p, "Sending...", now);
}

void pager_on_button(struct pager *p, enum button b, uint32_t now)
{
	enum item_action acts[4];
	uint8_t n;

	p->now = now;
	p->last_input = now;
	p->dirty = true;
	switch (p->screen) {
	case SCR_HOME:
		go(p, SCR_INBOX);
		break;
	case SCR_RINGING:
		if (b == BTN_UP) {
			haptic(p, HAPTIC_STOP);                  /* silence, let it ring out */
		} else {
			if (p->io.decline_call) p->io.decline_call(p->io.ctx);
			haptic(p, HAPTIC_STOP);
			if (b == BTN_SELECT && p->canned_count > 0) {
				go(p, SCR_REPLY);
				p->reply_live = true;
			} else {
				go(p, SCR_HOME);
			}
		}
		break;
	case SCR_INBOX:
		n = p->inbox_count + 1;                          /* + Back */
		if (b == BTN_UP) p->cursor = (p->cursor + n - 1) % n;
		if (b == BTN_DOWN) p->cursor = (p->cursor + 1) % n;
		if (b == BTN_SELECT) {
			if (p->cursor >= p->inbox_count) {
				go(p, SCR_HOME);
			} else {
				p->item = p->cursor;
				p->reply_live = false;
				go(p, SCR_ITEM);
			}
		}
		break;
	case SCR_ITEM:
		n = item_actions(&p->inbox[p->item], acts);
		if (b == BTN_UP) p->cursor = (p->cursor + n - 1) % n;
		if (b == BTN_DOWN) p->cursor = (p->cursor + 1) % n;
		if (b != BTN_SELECT) break;
		switch (acts[p->cursor]) {
		case ACT_REPLY:
			if (p->canned_count == 0) toast(p, "No replies synced", now);
			else go(p, SCR_REPLY);
			break;
		case ACT_READ:
			if (!p->link_up && p->vm_reading != p->inbox[p->item].vm_id) {
				toast(p, "No connection", now);
				break;
			}
			if (p->vm_reading != p->inbox[p->item].vm_id) {
				p->vm_reading = p->inbox[p->item].vm_id;
				p->vm_have = 0;
				p->vm_total = p->inbox[p->item].text_len;
				p->vm_text[0] = 0;
				vm_request(p);
			}
			go(p, SCR_VMTEXT);
			break;
		case ACT_DELETE:
			if (!p->link_up) {
				toast(p, "No connection", now);
				break;
			}
			vm_mark(p, p->inbox[p->item].vm_id, VM_DELETE);
			inbox_remove(p, p->item);
			go(p, SCR_INBOX);
			toast(p, "Deleted", now);
			break;
		default:
			go(p, SCR_INBOX);
			break;
		}
		break;
	case SCR_REPLY:
		n = p->canned_count + 1;                         /* + Cancel */
		if (b == BTN_UP) p->cursor = (p->cursor + n - 1) % n;
		if (b == BTN_DOWN) p->cursor = (p->cursor + 1) % n;
		if (b == BTN_SELECT) {
			if (p->cursor < p->canned_count) send_reply(p, p->cursor, now);
			go(p, SCR_HOME);
		}
		break;
	case SCR_VMTEXT:
		if (b == BTN_UP && p->top >= 3) p->top -= 3;
		else if (b == BTN_UP) p->top = 0;
		if (b == BTN_DOWN && p->top < 120) p->top += 3;
		if (b == BTN_SELECT) {
			uint8_t keep = p->item;

			go(p, SCR_ITEM);
			p->item = keep;
		}
		break;
	}
}

void pager_tick(struct pager *p, uint32_t now)
{
	uint32_t prev_min = (unix_now(p) + p->tz_min * 60) / 60;

	p->now = now;
	if (p->toast_until && now >= p->toast_until) {
		p->toast_until = 0;
		p->dirty = true;
	}
	if (p->screen != SCR_HOME && p->screen != SCR_RINGING && now - p->last_input >= IDLE_TIMEOUT_S) {
		go(p, SCR_HOME);
	}
	if (resync_left && now >= resync_at) {
		resync_left--;
		resync_at = now + RESYNC_NEXT_S;
		if (p->link_up) send_sync(p);
	}
	if ((unix_now(p) + p->tz_min * 60) / 60 != prev_min) p->dirty = true;    /* clock moved on */
}

/* ------------------------------------------------------------------ rendering */
static void put(char rows[UI_ROWS][UI_COLS + 1], int r, const char *fmt, ...)
{
	va_list ap;

	if (r < 0 || r >= UI_ROWS) return;
	va_start(ap, fmt);
	vsnprintf(rows[r], UI_COLS + 1, fmt, ap);
	va_end(ap);
}

static void age(const struct pager *p, uint32_t ts, char *out, size_t cap)
{
	uint32_t t = unix_now(p);
	uint32_t d = (t > ts) ? t - ts : 0;

	if (t == 0) snprintf(out, cap, "--");
	else if (d < 60) snprintf(out, cap, "now");
	else if (d < 3600) snprintf(out, cap, "%um", (unsigned)(d / 60));
	else if (d < 86400) snprintf(out, cap, "%uh", (unsigned)(d / 3600));
	else snprintf(out, cap, "%ud", (unsigned)(d / 86400));
}

/* Greedy word wrap. Returns the number of bytes consumed for one line. */
static size_t wrap(const char *s, char *line)
{
	size_t n = 0, last_space = 0;

	while (s[n] && s[n] != '\n' && n < UI_COLS) {
		if (s[n] == ' ') last_space = n;
		n++;
	}
	if (s[n] && s[n] != '\n' && s[n] != ' ' && last_space > 0) n = last_space;
	memcpy(line, s, n);
	line[n] = 0;
	while (s[n] == ' ' || s[n] == '\n') n++;
	return n;
}

static void wrapped(char rows[UI_ROWS][UI_COLS + 1], int first_row, int max_rows, const char *utf8,
		    int skip)
{
	char clean[VM_TEXT_MAX + 1];
	char line[UI_COLS + 1];
	const char *s = clean;
	int r = 0;

	ascii_copy(clean, sizeof(clean), utf8);
	while (*s && r < max_rows) {
		s += wrap(s, line);
		if (skip > 0) {
			skip--;
			continue;
		}
		put(rows, first_row + r++, "%s", line);
	}
}

void pager_render(const struct pager *p, char rows[UI_ROWS][UI_COLS + 1], int *invert)
{
	static const char *const link_name[] = { "BT", "LTE" };
	const struct inbox_item *it = &p->inbox[p->item < INBOX_MAX ? p->item : 0];
	enum item_action acts[4];
	char when[8], status[12];
	uint32_t local = unix_now(p) + (uint32_t)(p->tz_min * 60);
	uint8_t n, first;

	for (int r = 0; r < UI_ROWS; r++) rows[r][0] = 0;
	*invert = -1;

	snprintf(status, sizeof(status), "%s %u%%", p->link_up ? link_name[p->link ? 1 : 0] : "--",
		 p->batt_pct);
	if (unix_now(p)) {
		put(rows, 0, "%-15s %02u:%02u", status, (unsigned)((local / 3600) % 24),
		    (unsigned)((local / 60) % 60));
	} else {
		put(rows, 0, "%-15s --:--", status);
	}

	switch (p->screen) {
	case SCR_HOME:
		if (p->unheard) put(rows, 3, "%u new voicemail%s", p->unheard, p->unheard == 1 ? "" : "s");
		else put(rows, 3, "No new voicemail");
		if (p->inbox_count) {
			age(p, p->inbox[0].ts, when, sizeof(when));
			put(rows, 5, "Last: %.15s", who(&p->inbox[0]));
			put(rows, 6, "      %s%s", when, strcmp(when, "now") && strcmp(when, "--") ? " ago" : "");
		}
		break;
	case SCR_RINGING:
		put(rows, 2, "Incoming call");
		put(rows, 4, "%s", p->ring_who);
		put(rows, 6, "^ Silence");
		put(rows, 7, "o Decline + reply");
		put(rows, 8, "v Decline");
		break;
	case SCR_INBOX:
		n = p->inbox_count + 1;
		first = p->cursor >= UI_ROWS - 1 ? p->cursor - (UI_ROWS - 2) : 0;
		for (uint8_t i = first; i < n && i - first < UI_ROWS - 1; i++) {
			if (i == p->inbox_count) {
				put(rows, 1 + i - first, "< Back");
			} else {
				age(p, p->inbox[i].ts, when, sizeof(when));
				put(rows, 1 + i - first, "%c%-15.15s %4s",
				    p->inbox[i].is_vm ? (p->inbox[i].heard ? 'v' : 'V') : ' ',
				    who(&p->inbox[i]), when);
			}
			if (i == p->cursor) *invert = 1 + i - first;
		}
		break;
	case SCR_ITEM:
		age(p, it->ts, when, sizeof(when));
		put(rows, 1, "%s", who(it));
		if (it->name[0]) put(rows, 2, "%s", it->number);
		if (it->is_vm) put(rows, 3, "Voicemail %us  %s", it->duration_s, when);
		else put(rows, 3, "%s  %s", it->replied ? "Replied" : "Missed call", when);
		n = item_actions(it, acts);
		for (uint8_t i = 0; i < n; i++) {
			static const char *const label[] = { "Reply by text", "Read message", "Delete", "< Back" };

			put(rows, 5 + i, "%s", label[acts[i]]);
			if (i == p->cursor) *invert = 5 + i;
		}
		break;
	case SCR_REPLY:
		put(rows, 1, "Reply to %.12s", p->reply_live ? p->ring_who : who(it));
		n = p->canned_count + 1;
		first = p->cursor >= UI_ROWS - 2 ? p->cursor - (UI_ROWS - 3) : 0;
		for (uint8_t i = first; i < n && i - first < UI_ROWS - 2; i++) {
			put(rows, 2 + i - first, "%s", i == p->canned_count ? "< Cancel" : p->canned[i]);
			if (i == p->cursor) *invert = 2 + i - first;
		}
		break;
	case SCR_VMTEXT:
		put(rows, 1, "%.14s %us", who(it), it->duration_s);
		if (p->vm_have == 0) put(rows, 3, p->vm_total ? "Loading..." : "(no transcript)");
		else wrapped(rows, 2, UI_ROWS - 2, p->vm_text, p->top);
		break;
	}
	if (p->toast_until) {
		put(rows, UI_ROWS - 1, "%s", p->toast);
		*invert = UI_ROWS - 1;
	}
}
