#include "proto.h"

#include <stdio.h>
#include <string.h>

struct wr { uint8_t *p; size_t cap, n; int err; };
struct rd { const uint8_t *p; size_t len, n; int err; };

static void w8(struct wr *w, uint8_t v)
{
	if (w->n + 1 > w->cap) { w->err = 1; return; }
	w->p[w->n++] = v;
}
static void w16(struct wr *w, uint16_t v) { w8(w, v & 0xff); w8(w, v >> 8); }
static void w32(struct wr *w, uint32_t v) { w16(w, v & 0xffff); w16(w, v >> 16); }
static void wstr(struct wr *w, const char *s, size_t max)
{
	size_t l = strlen(s);

	if (l > max || l > 255) { w->err = 1; return; }
	w8(w, (uint8_t)l);
	if (w->n + l > w->cap) { w->err = 1; return; }
	memcpy(w->p + w->n, s, l);
	w->n += l;
}

static uint8_t r8(struct rd *r)
{
	if (r->n + 1 > r->len) { r->err = 1; return 0; }
	return r->p[r->n++];
}
static uint16_t r16(struct rd *r) { uint16_t lo = r8(r); return lo | ((uint16_t)r8(r) << 8); }
static uint32_t r32(struct rd *r) { uint32_t lo = r16(r); return lo | ((uint32_t)r16(r) << 16); }
static void rstr(struct rd *r, char *out, size_t max)
{
	size_t l = r8(r);

	out[0] = 0;
	if (r->err) return;
	if (l > max || r->n + l > r->len) { r->err = 1; return; }
	memcpy(out, r->p + r->n, l);
	out[l] = 0;
	if (memchr(out, 0, l) != NULL) { r->err = 1; return; }   /* no embedded NULs */
	r->n += l;
}

int proto_encode(const struct proto_msg *m, uint8_t *buf, size_t cap)
{
	struct wr w = { buf, cap < PROTO_MAX_FRAME ? cap : PROTO_MAX_FRAME, 0, 0 };

	w8(&w, m->type);
	w8(&w, m->seq);
	switch (m->type) {
	case PT_HELLO:
		w8(&w, m->u.hello.proto); w8(&w, m->u.hello.fw_major); w8(&w, m->u.hello.fw_minor);
		w8(&w, m->u.hello.batt_pct); w8(&w, m->u.hello.link);
		w16(&w, m->u.hello.cfg_ver); w32(&w, m->u.hello.last_event);
		break;
	case PT_SYNC: w32(&w, m->u.sync.last_event); break;
	case PT_REPLY: w32(&w, m->u.reply.call_event); w8(&w, m->u.reply.canned_idx); break;
	case PT_VM_GET: w32(&w, m->u.vm_get.vm_id); w16(&w, m->u.vm_get.offset); break;
	case PT_VM_MARK: w32(&w, m->u.vm_mark.vm_id); w8(&w, m->u.vm_mark.action); break;
	case PT_TIME: w32(&w, m->u.time.unix_time); w16(&w, (uint16_t)m->u.time.tz_min); break;
	case PT_CANNED:
		w16(&w, m->u.canned.cfg_ver); w8(&w, m->u.canned.idx); w8(&w, m->u.canned.count);
		wstr(&w, m->u.canned.text, PROTO_MAX_STR);
		break;
	case PT_EV_CALL:
		w32(&w, m->u.ev_call.event); w32(&w, m->u.ev_call.ts); w8(&w, m->u.ev_call.flags);
		wstr(&w, m->u.ev_call.number, PROTO_MAX_NUMBER);
		wstr(&w, m->u.ev_call.name, PROTO_MAX_NAME);
		break;
	case PT_EV_VM:
		w32(&w, m->u.ev_vm.event); w32(&w, m->u.ev_vm.vm_id); w32(&w, m->u.ev_vm.ts);
		w16(&w, m->u.ev_vm.duration_s); w16(&w, m->u.ev_vm.text_len);
		wstr(&w, m->u.ev_vm.number, PROTO_MAX_NUMBER);
		wstr(&w, m->u.ev_vm.name, PROTO_MAX_NAME);
		break;
	case PT_VM_TEXT:
		w32(&w, m->u.vm_text.vm_id); w16(&w, m->u.vm_text.offset); w16(&w, m->u.vm_text.total);
		wstr(&w, m->u.vm_text.chunk, PROTO_MAX_STR);
		break;
	case PT_REPLY_RESULT:
		w32(&w, m->u.reply_result.call_event); w8(&w, m->u.reply_result.status);
		break;
	case PT_SYNC_DONE:
		w32(&w, m->u.sync_done.last_event); w8(&w, m->u.sync_done.unheard);
		break;
	default:
		return -2;
	}
	return w.err ? -1 : (int)w.n;
}

int proto_decode(const uint8_t *buf, size_t len, struct proto_msg *m)
{
	struct rd r = { buf, len, 0, 0 };

	if (len < 2 || len > PROTO_MAX_FRAME) return -1;
	memset(m, 0, sizeof(*m));
	m->type = r8(&r);
	m->seq = r8(&r);
	switch (m->type) {
	case PT_HELLO:
		m->u.hello.proto = r8(&r); m->u.hello.fw_major = r8(&r); m->u.hello.fw_minor = r8(&r);
		m->u.hello.batt_pct = r8(&r); m->u.hello.link = r8(&r);
		m->u.hello.cfg_ver = r16(&r); m->u.hello.last_event = r32(&r);
		break;
	case PT_SYNC: m->u.sync.last_event = r32(&r); break;
	case PT_REPLY: m->u.reply.call_event = r32(&r); m->u.reply.canned_idx = r8(&r); break;
	case PT_VM_GET: m->u.vm_get.vm_id = r32(&r); m->u.vm_get.offset = r16(&r); break;
	case PT_VM_MARK: m->u.vm_mark.vm_id = r32(&r); m->u.vm_mark.action = r8(&r); break;
	case PT_TIME: m->u.time.unix_time = r32(&r); m->u.time.tz_min = (int16_t)r16(&r); break;
	case PT_CANNED:
		m->u.canned.cfg_ver = r16(&r); m->u.canned.idx = r8(&r); m->u.canned.count = r8(&r);
		rstr(&r, m->u.canned.text, PROTO_MAX_STR);
		break;
	case PT_EV_CALL:
		m->u.ev_call.event = r32(&r); m->u.ev_call.ts = r32(&r); m->u.ev_call.flags = r8(&r);
		rstr(&r, m->u.ev_call.number, PROTO_MAX_NUMBER);
		rstr(&r, m->u.ev_call.name, PROTO_MAX_NAME);
		break;
	case PT_EV_VM:
		m->u.ev_vm.event = r32(&r); m->u.ev_vm.vm_id = r32(&r); m->u.ev_vm.ts = r32(&r);
		m->u.ev_vm.duration_s = r16(&r); m->u.ev_vm.text_len = r16(&r);
		rstr(&r, m->u.ev_vm.number, PROTO_MAX_NUMBER);
		rstr(&r, m->u.ev_vm.name, PROTO_MAX_NAME);
		break;
	case PT_VM_TEXT:
		m->u.vm_text.vm_id = r32(&r); m->u.vm_text.offset = r16(&r); m->u.vm_text.total = r16(&r);
		rstr(&r, m->u.vm_text.chunk, PROTO_MAX_STR);
		break;
	case PT_REPLY_RESULT:
		m->u.reply_result.call_event = r32(&r); m->u.reply_result.status = r8(&r);
		break;
	case PT_SYNC_DONE:
		m->u.sync_done.last_event = r32(&r); m->u.sync_done.unheard = r8(&r);
		break;
	default:
		return -2;
	}
	if (r.err) return -1;
	if (r.n != len) return -3;   /* trailing bytes */
	return 0;
}

int proto_dump(const struct proto_msg *m, char *o, size_t cap)
{
	switch (m->type) {
	case PT_HELLO:
		return snprintf(o, cap, "HELLO seq=%u proto=%u fw=%u.%u batt=%u link=%u cfg=%u last=%u",
				m->seq, m->u.hello.proto, m->u.hello.fw_major, m->u.hello.fw_minor,
				m->u.hello.batt_pct, m->u.hello.link, m->u.hello.cfg_ver,
				(unsigned)m->u.hello.last_event);
	case PT_SYNC:
		return snprintf(o, cap, "SYNC seq=%u last=%u", m->seq, (unsigned)m->u.sync.last_event);
	case PT_REPLY:
		return snprintf(o, cap, "REPLY seq=%u call=%u idx=%u", m->seq,
				(unsigned)m->u.reply.call_event, m->u.reply.canned_idx);
	case PT_VM_GET:
		return snprintf(o, cap, "VM_GET seq=%u vm=%u offset=%u", m->seq,
				(unsigned)m->u.vm_get.vm_id, m->u.vm_get.offset);
	case PT_VM_MARK:
		return snprintf(o, cap, "VM_MARK seq=%u vm=%u action=%u", m->seq,
				(unsigned)m->u.vm_mark.vm_id, m->u.vm_mark.action);
	case PT_TIME:
		return snprintf(o, cap, "TIME seq=%u unix=%u tz=%d", m->seq,
				(unsigned)m->u.time.unix_time, m->u.time.tz_min);
	case PT_CANNED:
		return snprintf(o, cap, "CANNED seq=%u cfg=%u idx=%u count=%u text=[%s]", m->seq,
				m->u.canned.cfg_ver, m->u.canned.idx, m->u.canned.count, m->u.canned.text);
	case PT_EV_CALL:
		return snprintf(o, cap, "EV_CALL seq=%u event=%u ts=%u flags=%u number=[%s] name=[%s]",
				m->seq, (unsigned)m->u.ev_call.event, (unsigned)m->u.ev_call.ts,
				m->u.ev_call.flags, m->u.ev_call.number, m->u.ev_call.name);
	case PT_EV_VM:
		return snprintf(o, cap,
				"EV_VM seq=%u event=%u vm=%u ts=%u dur=%u len=%u number=[%s] name=[%s]",
				m->seq, (unsigned)m->u.ev_vm.event, (unsigned)m->u.ev_vm.vm_id,
				(unsigned)m->u.ev_vm.ts, m->u.ev_vm.duration_s, m->u.ev_vm.text_len,
				m->u.ev_vm.number, m->u.ev_vm.name);
	case PT_VM_TEXT:
		return snprintf(o, cap, "VM_TEXT seq=%u vm=%u offset=%u total=%u chunk=[%s]", m->seq,
				(unsigned)m->u.vm_text.vm_id, m->u.vm_text.offset, m->u.vm_text.total,
				m->u.vm_text.chunk);
	case PT_REPLY_RESULT:
		return snprintf(o, cap, "REPLY_RESULT seq=%u call=%u status=%u", m->seq,
				(unsigned)m->u.reply_result.call_event, m->u.reply_result.status);
	case PT_SYNC_DONE:
		return snprintf(o, cap, "SYNC_DONE seq=%u last=%u unheard=%u", m->seq,
				(unsigned)m->u.sync_done.last_event, m->u.sync_done.unheard);
	default:
		return -1;
	}
}
