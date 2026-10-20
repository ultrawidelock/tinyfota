/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file test_tinyble.c — the update channel on tinyble, over real zcbor.
 *
 * Files under test: ports/zephyr/dfu_tinyble.c (SMP framing, the os group,
 * notifications, the native frames), ports/zephyr/smp_decode.c (the request
 * map decoder), and ports/zephyr/smp_group.c reached the way a client
 * reaches it: SMP request bytes written to the SMP value, replies read back
 * as the bytes notified. zcbor is the real library (make tinyble ZCBOR=...);
 * tinyble and the system work queue are doubles (tests/host/tbfake), the
 * receiver and flash are the same host port the other suites use.
 *
 * Requests are encoded here by a few lines of definite-length CBOR, the way
 * `tinyfota push` and the phone apps send them; replies are compared byte for
 * byte against zcbor's indefinite-length maps, which is what mcumgr sends.
 */
#include <stdio.h>
#include <string.h>

#include "dfufake.h"
#include "psafake.h"
#include "tbfake.h"
#include "test.h"

#include <tinyble/tb_dfu.h>
#include <tinyble/tb_zephyr.h>

#include "dfu_crc.h"
#include "tinyfota.h"
#include "tinyfota/tinyfota_port.h"
#include "tinyfota_host.h"
#include "tinyfota_rx.h"

/* dfu_receiver.c links this from a generated file on target. The fake PSA
 * never looks at it. */
const uint8_t tinyfota_pubkey[65] = {0x04};

#define HEAD_LEN (TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN)

#define OP_READ      0u
#define OP_READ_RSP  1u
#define OP_WRITE     2u
#define OP_WRITE_RSP 3u

/* ---- requests: just enough CBOR ------------------------------------------- */

struct cb {
	uint8_t buf[1200];
	size_t n;
};

static void cb_head(struct cb *b, uint8_t major, uint32_t v)
{
	if (v < 24u) {
		b->buf[b->n++] = (uint8_t)((major << 5) | v);
	} else if (v < 0x100u) {
		b->buf[b->n++] = (uint8_t)((major << 5) | 24u);
		b->buf[b->n++] = (uint8_t)v;
	} else if (v < 0x10000u) {
		b->buf[b->n++] = (uint8_t)((major << 5) | 25u);
		b->buf[b->n++] = (uint8_t)(v >> 8);
		b->buf[b->n++] = (uint8_t)v;
	} else {
		b->buf[b->n++] = (uint8_t)((major << 5) | 26u);
		b->buf[b->n++] = (uint8_t)(v >> 24);
		b->buf[b->n++] = (uint8_t)(v >> 16);
		b->buf[b->n++] = (uint8_t)(v >> 8);
		b->buf[b->n++] = (uint8_t)v;
	}
}

static void cb_byte(struct cb *b, uint8_t v)
{
	b->buf[b->n++] = v;
}

static void cb_uint(struct cb *b, uint32_t v)
{
	cb_head(b, 0, v);
}

static void cb_bstr(struct cb *b, const uint8_t *p, size_t len)
{
	cb_head(b, 2, (uint32_t)len);
	memcpy(&b->buf[b->n], p, len);
	b->n += len;
}

static void cb_tstr(struct cb *b, const char *s)
{
	const size_t len = strlen(s);

	cb_head(b, 3, (uint32_t)len);
	memcpy(&b->buf[b->n], s, len);
	b->n += len;
}

static void cb_map(struct cb *b, uint32_t pairs)
{
	cb_head(b, 5, pairs);
}

/** One SMP packet: the 8-byte header in front of @p body. */
static size_t smp_packet(uint8_t *out, uint8_t op, uint8_t ver, uint16_t group, uint8_t seq,
			 uint8_t id, const struct cb *body)
{
	const size_t len = (body != NULL) ? body->n : 0u;

	out[0] = (uint8_t)(op | (ver << 3));
	out[1] = 0;
	out[2] = (uint8_t)(len >> 8);
	out[3] = (uint8_t)len;
	out[4] = (uint8_t)(group >> 8);
	out[5] = (uint8_t)group;
	out[6] = seq;
	out[7] = id;
	if (len > 0u) {
		memcpy(out + 8, body->buf, len);
	}
	return 8u + len;
}

/* ---- the radio side ---------------------------------------------------------- */

/** Write @p len bytes to @p handle in writes of at most @p chunk, as tinyble
 *  would call the hook: on its own context, with its lock held. */
static uint8_t write_in(uint16_t handle, const uint8_t *p, size_t len, size_t chunk)
{
	uint8_t att;

	do {
		const size_t n = (len < chunk) ? len : chunk;

		tb_zephyr_lock();
		att = tb_dfu_on_write(handle, p, (uint16_t)n);
		tb_zephyr_unlock();
		p += n;
		len -= n;
	} while (len > 0u);
	return att;
}

static uint8_t pkt[1200];
static uint8_t rsp[1200];

/** Send one request in one write, let the work queue run, return the reply. */
static size_t call(uint8_t op, uint8_t ver, uint16_t group, uint8_t seq, uint8_t id,
		   const struct cb *body)
{
	const size_t n = smp_packet(pkt, op, ver, group, seq, id, body);
	size_t got;

	tbfake_clear_notify();
	(void)write_in(TB_DFU_H_SMP, pkt, n, n);
	(void)tbfake_advance_ms(20);
	got = tbfake_stream(TB_DFU_H_SMP, rsp, sizeof(rsp));
	return got;
}

static void ready(void)
{
	dfufake_reset();
	psafake_reset();
	tinyfota_rx_reset_all();
	tinyfota_window_close();
	tb_zephyr_lock();
	tb_dfu_on_disconnect(0x13);
	tb_zephyr_unlock();
	tbfake_reset();
}

static void expect_bytes(const char *name, const uint8_t *got, size_t got_len, const uint8_t *want,
			 size_t want_len)
{
	const int same = got_len == want_len && memcmp(got, want, want_len) == 0;

	T_OK(name, same);
	if (!same) {
		static char hex[2 * sizeof(rsp) + 1];

		t_hex(hex, got, got_len);
		printf("      got  %s\n", hex);
		t_hex(hex, want, want_len);
		printf("      want %s\n", hex);
	}
}

/** The header a reply to (op, ver, group, seq, id) must carry, then CBOR. */
static size_t reply_head(uint8_t *out, uint8_t rsp_op, uint8_t ver, uint16_t group, uint8_t seq,
			 uint8_t id, size_t body_len)
{
	out[0] = (uint8_t)(rsp_op | (ver << 3));
	out[1] = 0;
	out[2] = (uint8_t)(body_len >> 8);
	out[3] = (uint8_t)body_len;
	out[4] = (uint8_t)(group >> 8);
	out[5] = (uint8_t)group;
	out[6] = seq;
	out[7] = id;
	return 8;
}

/** {"rc": rc} as mcumgr's error reply encodes it. */
static size_t error_reply(uint8_t *out, uint8_t rsp_op, uint8_t ver, uint16_t group, uint8_t seq,
			  uint8_t id, uint8_t rc)
{
	const uint8_t body[] = {0xbf, 0x62, 'r', 'c', rc, 0xff};

	reply_head(out, rsp_op, ver, group, seq, id, sizeof(body));
	memcpy(out + 8, body, sizeof(body));
	return 8 + sizeof(body);
}

static void expect_error(const char *name, size_t got, uint8_t rsp_op, uint8_t ver, uint16_t group,
			 uint8_t seq, uint8_t id, uint8_t rc)
{
	uint8_t want[16];
	const size_t n = error_reply(want, rsp_op, ver, group, seq, id, rc);

	expect_bytes(name, rsp, got, want, n);
}

/* ---- the running image and an update ----------------------------------------- */

static uint8_t s_sha[32];

/** An MCUboot image at the slot0 address: a header, 64 B, the TLV block. */
static void lay_out_image(uint8_t major, uint8_t minor, uint16_t rev, uint32_t build)
{
	uint8_t *p = dfufake_running_image;
	uint8_t *tlv = p + 32 + 64;

	for (size_t i = 0; i < sizeof(s_sha); i++) {
		s_sha[i] = (uint8_t)(0x40 + i);
	}
	memset(p, 0, DFUFAKE_PAGE_SIZE);
	tinyfota_put_le32(0x96f3b83du, p);
	p[8] = 32;   /* ih_hdr_size */
	p[12] = 64;  /* ih_img_size */
	p[20] = major;
	p[21] = minor;
	p[22] = (uint8_t)rev;
	p[23] = (uint8_t)(rev >> 8);
	tinyfota_put_le32(build, &p[24]);
	tlv[0] = 0x07; /* IMAGE_TLV_INFO_MAGIC 0x6907 */
	tlv[1] = 0x69;
	tlv[2] = 4 + 4 + 32;
	tlv[4] = 0x10; /* SHA-256 */
	tlv[6] = 32;
	memcpy(tlv + 8, s_sha, sizeof(s_sha));
}

static uint8_t s_patch[300];
static uint8_t s_wire[HEAD_LEN + sizeof(s_patch)];

static void build_wire(void)
{
	struct tinyfota_hdr hdr;
	uint8_t head[HEAD_LEN];

	for (size_t i = 0; i < sizeof(s_patch); i++) {
		s_patch[i] = (uint8_t)(0x11 + i * 7u);
	}
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = TINYFOTA_MAGIC;
	hdr.abi_version = TINYFOTA_ABI_VERSION;
	hdr.patch_len = sizeof(s_patch);
	hdr.patch_crc32 = tinyfota_crc32(s_patch, sizeof(s_patch));
	memcpy(head, &hdr, sizeof(hdr));
	hdr.hdr_crc32 = tinyfota_crc32(head, TINYFOTA_HDR_CRC_LEN);
	memcpy(head, &hdr, sizeof(hdr));
	memset(head + TINYFOTA_HDR_LEN, 0xa5, TINYFOTA_SIG_LEN);
	memcpy(s_wire, head, HEAD_LEN);
	memcpy(s_wire + HEAD_LEN, s_patch, sizeof(s_patch));
}

static void upload_body(struct cb *b, size_t off, size_t len)
{
	b->n = 0;
	cb_map(b, off == 0u ? 4 : 3);
	cb_tstr(b, "image");
	cb_uint(b, 0);
	cb_tstr(b, "off");
	cb_uint(b, (uint32_t)off);
	if (off == 0u) {
		cb_tstr(b, "len");
		cb_uint(b, sizeof(s_wire));
	}
	cb_tstr(b, "data");
	cb_bstr(b, s_wire + off, len);
}

/** {"off": next}, the reply to an accepted chunk. */
static size_t off_reply(uint8_t *out, uint8_t seq, uint32_t next)
{
	struct cb b = {0};

	cb_byte(&b, 0xbf);
	cb_tstr(&b, "off");
	cb_uint(&b, next);
	cb_byte(&b, 0xff);
	reply_head(out, OP_WRITE_RSP, 0, 1, seq, 1, b.n);
	memcpy(out + 8, b.buf, b.n);
	return 8 + b.n;
}

/** Upload the whole wire in @p chunk byte pieces; returns the checks' verdict. */
static bool upload_all(size_t chunk)
{
	uint8_t want[32];
	size_t off = 0;
	uint8_t seq = 0x40;
	bool ok = true;

	while (off < sizeof(s_wire)) {
		const size_t n = (sizeof(s_wire) - off < chunk) ? sizeof(s_wire) - off : chunk;
		struct cb b;
		size_t got;
		size_t wn;

		upload_body(&b, off, n);
		got = call(OP_WRITE, 0, 1, seq, 1, &b);
		wn = off_reply(want, seq, (uint32_t)(off + n));
		ok = ok && got == wn && memcmp(rsp, want, wn) == 0;
		off += n;
		seq++;
	}
	return ok;
}

/* ---- image group ------------------------------------------------------------- */

static size_t state_reply(uint8_t *out, uint8_t seq, const char *vers)
{
	struct cb b = {0};

	cb_byte(&b, 0xbf);
	cb_tstr(&b, "images");
	cb_byte(&b, 0x9f);
	cb_byte(&b, 0xbf);
	cb_tstr(&b, "image");
	cb_uint(&b, 0);
	cb_tstr(&b, "slot");
	cb_uint(&b, 0);
	cb_tstr(&b, "version");
	cb_tstr(&b, vers);
	cb_tstr(&b, "hash");
	cb_bstr(&b, s_sha, sizeof(s_sha));
	cb_tstr(&b, "bootable");
	cb_byte(&b, 0xf5);
	cb_tstr(&b, "pending");
	cb_byte(&b, 0xf4);
	cb_tstr(&b, "confirmed");
	cb_byte(&b, 0xf5);
	cb_tstr(&b, "active");
	cb_byte(&b, 0xf5);
	cb_tstr(&b, "permanent");
	cb_byte(&b, 0xf5);
	cb_byte(&b, 0xff);
	cb_byte(&b, 0xff);
	cb_tstr(&b, "splitStatus");
	cb_uint(&b, 0);
	cb_byte(&b, 0xff);
	reply_head(out, OP_READ_RSP, 0, 1, seq, 0, b.n);
	memcpy(out + 8, b.buf, b.n);
	return 8 + b.n;
}

static void test_state(void)
{
	struct cb empty = {0};
	uint8_t want[256];
	size_t got;
	size_t n;

	t_group("tinyble smp image state");
	cb_map(&empty, 0);

	ready();
	lay_out_image(1, 2, 3, 4);
	got = call(OP_READ, 0, 1, 0x21, 0, &empty);
	n = state_reply(want, 0x21, "1.2.3.4");
	expect_bytes("image list, byte for byte", rsp, got, want, n);
	T_EQ("split at MTU 23: 20 B a notification", (long)tbfake.notify[0].len, 20L);
	T_EQ("no notification over MTU - 3", (long)tbfake.oversize_calls, 0L);
	T_EQ("every notification under tinyble's lock", (long)tbfake.unlocked_calls, 0L);
	T_EQ("lock released", (long)tbfake.lock_depth, 0L);

	/* The longest version string fits: the reply buffer is sized for it. */
	ready();
	lay_out_image(255, 255, 65535, 4294967295u);
	got = call(OP_READ, 0, 1, 0x22, 0, &empty);
	n = state_reply(want, 0x22, "255.255.65535.429496729");
	expect_bytes("longest image list", rsp, got, want, n);
	T_EQ("longest image list is 167 B", (long)got, 167L);

	/* At a 247 B MTU it is one notification. */
	ready();
	lay_out_image(1, 2, 3, 4);
	tbfake.mtu = 247;
	got = call(OP_READ, 0, 1, 0x23, 0, &empty);
	T_EQ("one notification at MTU 247", (long)tbfake.notify_count, 1L);
	T_EQ("whole reply in it", (long)got, (long)state_reply(want, 0x23, "1.2.3.4"));

	/* State write: accepted, nothing marked, answered with the list. */
	ready();
	lay_out_image(1, 2, 3, 4);
	{
		struct cb b = {0};

		cb_map(&b, 2);
		cb_tstr(&b, "hash");
		cb_bstr(&b, s_sha, sizeof(s_sha));
		cb_tstr(&b, "confirm");
		cb_byte(&b, 0xf5);
		got = call(OP_WRITE, 0, 1, 0x24, 0, &b);
		n = state_reply(want, 0x24, "1.2.3.4");
		want[0] = OP_WRITE_RSP;
		expect_bytes("state write answers with the list", rsp, got, want, n);
	}

	/* SMP version 2 (header version 1) is answered in kind. */
	ready();
	lay_out_image(1, 2, 3, 4);
	got = call(OP_READ, 1, 1, 0x25, 0, &empty);
	T_EQ("version 1 reply", (long)(got > 0 ? rsp[0] : 0xff), (long)(OP_READ_RSP | (1u << 3)));
	/* A version past it is refused, as mcumgr refuses it. */
	got = call(OP_READ, 2, 1, 0x26, 0, &empty);
	expect_error("version 2 refused, too new", got, OP_READ_RSP, 1, 1, 0x26, 0, 13);
}

static void test_upload(void)
{
	struct cb b;
	uint8_t want[32];
	size_t got;
	size_t n;
	const unsigned reboots = tinyfota_flash_host_reboots();

	t_group("tinyble smp upload");
	build_wire();

	/* The window is shut: access denied, as the mcumgr build says it. */
	ready();
	upload_body(&b, 0, 64);
	got = call(OP_WRITE, 0, 1, 0x30, 1, &b);
	expect_error("upload refused while closed", got, OP_WRITE_RSP, 0, 1, 0x30, 1, 11);
	T_EQ("nothing erased", (long)dfufake_staging.erase_calls, 0L);

	/* Open: the whole file in 100 B chunks, the position echoed each time. */
	ready();
	tinyfota_window_open(100000);
	T_OK("every chunk answered with the next offset", upload_all(100));
	T_OK("staged", tinyfota_rx_staged());
	T_EQ("staged, not rebooted: the client sends reset", (long)tinyfota_flash_host_reboots(),
	     (long)reboots);

	/* A request longer than one write: 200 B of data at MTU 23 is eleven
	 * writes, reassembled by the header's length. The header itself arrives
	 * split too. */
	ready();
	tinyfota_window_open(100000);
	upload_body(&b, 0, 200);
	n = smp_packet(pkt, OP_WRITE, 0, 1, 0x31, 1, &b);
	tbfake_clear_notify();
	(void)write_in(TB_DFU_H_SMP, pkt, 3, 3);
	(void)tbfake_advance_ms(5);
	T_EQ("nothing answered for a partial header", (long)tbfake.notify_count, 0L);
	(void)write_in(TB_DFU_H_SMP, pkt + 3, n - 3, 20);
	(void)tbfake_advance_ms(20);
	got = tbfake_stream(TB_DFU_H_SMP, rsp, sizeof(rsp));
	expect_bytes("reassembled request answered", rsp, got, want, off_reply(want, 0x31, 200));

	/* A stale offset is a resync, reported as the real position. */
	upload_body(&b, 150, 20);
	got = call(OP_WRITE, 0, 1, 0x32, 1, &b);
	expect_bytes("stale offset resyncs", rsp, got, want, off_reply(want, 0x32, 200));

	/* A dropped link releases the claim: a reconnecting client is sent
	 * back to 0. */
	tb_zephyr_lock();
	tb_dfu_on_disconnect(0x08);
	tb_zephyr_unlock();
	upload_body(&b, 200, 20);
	got = call(OP_WRITE, 0, 1, 0x33, 1, &b);
	expect_bytes("after a disconnect, start again at 0", rsp, got, want,
		     off_reply(want, 0x33, 0));

	/* A refusal from the receiver: bad state, nothing staged. */
	ready();
	tinyfota_window_open(100000);
	psafake.verify_ret = -1;
	upload_body(&b, 0, 120);
	got = call(OP_WRITE, 0, 1, 0x34, 1, &b);
	expect_error("bad signature refused", got, OP_WRITE_RSP, 0, 1, 0x34, 1, 6);
	T_OK("nothing staged", !tinyfota_rx_staged());

	/* Erase: behind the window, and then it throws the staged update away. */
	ready();
	{
		struct cb e = {0};

		cb_map(&e, 0);
		got = call(OP_WRITE, 0, 1, 0x35, 5, &e);
		expect_error("erase refused while closed", got, OP_WRITE_RSP, 0, 1, 0x35, 5, 11);
		tinyfota_window_open(100000);
		T_OK("stage one", upload_all(160) && tinyfota_rx_staged());
		got = call(OP_WRITE, 0, 1, 0x36, 5, &e);
		{
			const uint8_t ok_body[] = {0xbf, 0xff};

			reply_head(want, OP_WRITE_RSP, 0, 1, 0x36, 5, sizeof(ok_body));
			memcpy(want + 8, ok_body, sizeof(ok_body));
			expect_bytes("erase answered {}", rsp, got, want, 10);
		}
		T_OK("no longer staged", !tinyfota_rx_staged());
	}
}

/* ---- malformed and oversize --------------------------------------------------- */

static void test_malformed(void)
{
	struct cb b;
	size_t got;
	uint8_t list[] = {0x81, 0x81, 0x81, 0x01};

	t_group("tinyble smp malformed requests");
	build_wire();

	ready();
	tinyfota_window_open(100000);

	/* Not a map at all. */
	b.n = 0;
	cb_uint(&b, 1);
	got = call(OP_WRITE, 0, 1, 0x50, 1, &b);
	expect_error("body that is not a map", got, OP_WRITE_RSP, 0, 1, 0x50, 1, 3);

	/* A map cut short: the header promises more than the body holds. */
	upload_body(&b, 0, 40);
	b.n -= 10;
	got = call(OP_WRITE, 0, 1, 0x51, 1, &b);
	expect_error("truncated map", got, OP_WRITE_RSP, 0, 1, 0x51, 1, 3);

	/* "off" as text. */
	b.n = 0;
	cb_map(&b, 2);
	cb_tstr(&b, "off");
	cb_tstr(&b, "0");
	cb_tstr(&b, "data");
	cb_bstr(&b, s_wire, 8);
	got = call(OP_WRITE, 0, 1, 0x52, 1, &b);
	expect_error("value of the wrong type", got, OP_WRITE_RSP, 0, 1, 0x52, 1, 3);

	/* "off" twice. */
	b.n = 0;
	cb_map(&b, 3);
	cb_tstr(&b, "off");
	cb_uint(&b, 0);
	cb_tstr(&b, "off");
	cb_uint(&b, 0);
	cb_tstr(&b, "data");
	cb_bstr(&b, s_wire, 8);
	got = call(OP_WRITE, 0, 1, 0x53, 1, &b);
	expect_error("key given twice", got, OP_WRITE_RSP, 0, 1, 0x53, 1, 3);

	/* An integer key. */
	b.n = 0;
	cb_map(&b, 1);
	cb_uint(&b, 7);
	cb_uint(&b, 0);
	got = call(OP_WRITE, 0, 1, 0x54, 1, &b);
	expect_error("key that is not text", got, OP_WRITE_RSP, 0, 1, 0x54, 1, 3);

	/* An unknown key holding a nested list: refused, never recursed into. */
	upload_body(&b, 0, 16);
	b.buf[0]++; /* one more pair */
	cb_tstr(&b, "x");
	memcpy(&b.buf[b.n], list, sizeof(list));
	b.n += sizeof(list);
	got = call(OP_WRITE, 0, 1, 0x55, 1, &b);
	expect_error("nested value under an unknown key", got, OP_WRITE_RSP, 0, 1, 0x55, 1, 3);

	/* An unknown key holding a scalar is skipped and the chunk taken. */
	upload_body(&b, 0, 16);
	b.buf[0]++;
	cb_tstr(&b, "sha");
	cb_bstr(&b, s_wire, 4);
	b.buf[0]++;
	cb_tstr(&b, "unknown");
	cb_uint(&b, 70000);
	got = call(OP_WRITE, 0, 1, 0x56, 1, &b);
	{
		uint8_t want[32];

		expect_bytes("unknown scalar skipped", rsp, got, want, off_reply(want, 0x56, 16));
	}

	/* Missing "off". */
	b.n = 0;
	cb_map(&b, 1);
	cb_tstr(&b, "data");
	cb_bstr(&b, s_wire, 8);
	got = call(OP_WRITE, 0, 1, 0x57, 1, &b);
	expect_error("no offset", got, OP_WRITE_RSP, 0, 1, 0x57, 1, 3);

	/* A request too large to hold: ENOMEM by its header, its body skipped
	 * over three writes, and the request after it served normally. */
	ready();
	{
		uint8_t big[8 + 600];
		struct cb echo = {0};

		memset(big, 0x5a, sizeof(big));
		big[0] = OP_WRITE;
		big[1] = 0;
		big[2] = (uint8_t)(600 >> 8);
		big[3] = (uint8_t)600;
		big[4] = 0;
		big[5] = 0;
		big[6] = 0x58;
		big[7] = 0;
		tbfake_clear_notify();
		(void)write_in(TB_DFU_H_SMP, big, sizeof(big), 244);
		(void)tbfake_advance_ms(20);
		got = tbfake_stream(TB_DFU_H_SMP, rsp, sizeof(rsp));
		expect_error("oversize refused, ENOMEM", got, OP_WRITE_RSP, 0, 0, 0x58, 0, 2);

		cb_map(&echo, 1);
		cb_tstr(&echo, "d");
		cb_tstr(&echo, "next");
		got = call(OP_WRITE, 0, 0, 0x59, 0, &echo);
		T_OK("the next request is served",
		     got == 17 && memcmp(rsp + 8, "\xbf\x61r\x64next\xff", 9) == 0);
	}
}

/* ---- the os group and everything else ---------------------------------------- */

static void test_os(void)
{
	struct cb b;
	uint8_t want[256];
	size_t got;
	const uint8_t params[] = {0xbf, 0x68, 'b', 'u', 'f', '_', 's', 'i', 'z', 'e', 0x19, 0x01, 0x80,
				  0x69, 'b', 'u', 'f', '_', 'c', 'o', 'u', 'n', 't', 0x01, 0xff};
	char text[200];

	t_group("tinyble smp os group");

	ready();
	b.n = 0;
	cb_map(&b, 1);
	cb_tstr(&b, "d");
	cb_tstr(&b, "hello");
	got = call(OP_WRITE, 0, 0, 0x60, 0, &b);
	{
		const uint8_t body[] = {0xbf, 0x61, 'r', 0x65, 'h', 'e', 'l', 'l', 'o', 0xff};

		reply_head(want, OP_WRITE_RSP, 0, 0, 0x60, 0, sizeof(body));
		memcpy(want + 8, body, sizeof(body));
		expect_bytes("echo", rsp, got, want, 8 + sizeof(body));
	}
	got = call(OP_READ, 0, 0, 0x61, 0, &b);
	T_OK("echo as a read too", got == 18 && rsp[0] == OP_READ_RSP);

	b.n = 0;
	cb_map(&b, 0);
	got = call(OP_WRITE, 0, 0, 0x62, 0, &b);
	T_OK("echo without d answers an empty r",
	     got == 13 && memcmp(rsp + 8, "\xbf\x61r\x60\xff", 5) == 0);

	/* An echo too long for the reply buffer: message size, as mcumgr says
	 * when its own buffer is short. 170 B fits, 180 B does not. */
	memset(text, 'e', sizeof(text));
	text[170] = '\0';
	b.n = 0;
	cb_map(&b, 1);
	cb_tstr(&b, "d");
	cb_tstr(&b, text);
	got = call(OP_WRITE, 0, 0, 0x63, 0, &b);
	T_EQ("170 B echo fits", (long)got, 8L + 1 + 2 + 2 + 170 + 1);
	text[170] = 'e';
	text[180] = '\0';
	b.n = 0;
	cb_map(&b, 1);
	cb_tstr(&b, "d");
	cb_tstr(&b, text);
	got = call(OP_WRITE, 0, 0, 0x64, 0, &b);
	expect_error("180 B echo refused", got, OP_WRITE_RSP, 0, 0, 0x64, 0, 7);

	/* mcumgr parameters: the reassembly buffer, one at a time. */
	b.n = 0;
	cb_map(&b, 0);
	got = call(OP_READ, 0, 0, 0x65, 6, &b);
	reply_head(want, OP_READ_RSP, 0, 0, 0x65, 6, sizeof(params));
	memcpy(want + 8, params, sizeof(params));
	expect_bytes("params: buf_size 384, buf_count 1", rsp, got, want, 8 + sizeof(params));

	/* Everything else is not supported, in mcumgr's words. */
	got = call(OP_READ, 0, 0, 0x66, 2, &b);
	expect_error("os taskstat", got, OP_READ_RSP, 0, 0, 0x66, 2, 8);
	got = call(OP_READ, 0, 9, 0x67, 0, &b);
	expect_error("unknown group", got, OP_READ_RSP, 0, 9, 0x67, 0, 8);
	got = call(OP_READ, 0, 1, 0x68, 1, &b);
	expect_error("upload as a read", got, OP_READ_RSP, 0, 1, 0x68, 1, 8);
	got = call(OP_READ, 0, 1, 0x69, 2, &b);
	expect_error("image file command", got, OP_READ_RSP, 0, 1, 0x69, 2, 8);
	got = call(OP_READ, 0, 0, 0x6a, 5, &b);
	expect_error("reset as a read", got, OP_READ_RSP, 0, 0, 0x6a, 5, 8);
	got = call(OP_WRITE, 0, 0, 0x6b, 6, &b);
	expect_error("params as a write", got, OP_WRITE_RSP, 0, 0, 0x6b, 6, 8);
	got = call(OP_WRITE_RSP, 0, 0, 0x6c, 0, &b);
	expect_error("a response sent to the board", got, OP_WRITE_RSP, 0, 0, 0x6c, 0, 8);
}

static void test_reset(void)
{
	struct cb b = {0};
	const uint8_t ok_body[] = {0xbf, 0xff};
	uint8_t want[16];
	size_t got;
	unsigned reboots;

	t_group("tinyble smp reset gate");
	cb_map(&b, 0);
	build_wire();
	reply_head(want, OP_WRITE_RSP, 0, 0, 0x70, 5, sizeof(ok_body));
	memcpy(want + 8, ok_body, sizeof(ok_body));

	/* THE CASE THE GATE EXISTS FOR: idle, anyone in range, refused. */
	ready();
	reboots = tinyfota_flash_host_reboots();
	got = call(OP_WRITE, 0, 0, 0x70, 5, &b);
	expect_error("reset refused when idle", got, OP_WRITE_RSP, 0, 0, 0x70, 5, 11);
	(void)tbfake_advance_ms(1000);
	T_EQ("and no reboot", (long)tinyfota_flash_host_reboots(), (long)reboots);

	/* The owner opened a window: the reply first, the reboot 250 ms later. */
	ready();
	reboots = tinyfota_flash_host_reboots();
	tinyfota_window_open(100000);
	got = call(OP_WRITE, 0, 0, 0x70, 5, &b);
	expect_bytes("reset allowed with the window open", rsp, got, want, 10);
	(void)tbfake_advance_ms(200);
	T_EQ("not before 250 ms", (long)tinyfota_flash_host_reboots(), (long)reboots);
	(void)tbfake_advance_ms(60);
	T_EQ("rebooted", (long)tinyfota_flash_host_reboots(), (long)reboots + 1);

	/* A verified update staged, the window since closed: allowed. */
	ready();
	reboots = tinyfota_flash_host_reboots();
	tinyfota_window_open(100000);
	T_OK("stage one", upload_all(200) && tinyfota_rx_staged());
	tinyfota_window_close();
	got = call(OP_WRITE, 0, 0, 0x70, 5, &b);
	expect_bytes("reset allowed with an update staged", rsp, got, want, 10);
	(void)tbfake_advance_ms(300);
	T_EQ("rebooted into the bootloader", (long)tinyfota_flash_host_reboots(), (long)reboots + 1);
}

/* ---- notifications ------------------------------------------------------------- */

static void test_notify(void)
{
	struct cb b = {0};
	uint8_t want[256];
	size_t got;

	t_group("tinyble notifications");
	cb_map(&b, 0);

	/* TB_EBUSY: nothing lost, retried until tinyble takes it. */
	ready();
	lay_out_image(1, 2, 3, 4);
	tbfake.busy_n = 5;
	got = call(OP_READ, 0, 1, 0x80, 0, &b);
	expect_bytes("reply survives TB_EBUSY", rsp, got, want, state_reply(want, 0x80, "1.2.3.4"));
	T_OK("retried", tbfake.calls > tbfake.notify_count);
	T_EQ("retries under the lock too", (long)tbfake.unlocked_calls, 0L);

	/* Notifications off: the reply is dropped, the next request served. */
	ready();
	lay_out_image(1, 2, 3, 4);
	tbfake.fail_ret = TB_EAGAIN;
	got = call(OP_READ, 0, 1, 0x81, 0, &b);
	T_EQ("nothing delivered", (long)got, 0L);
	tbfake.fail_ret = 0;
	got = call(OP_READ, 0, 1, 0x82, 0, &b);
	T_EQ("next request answered", (long)got, (long)state_reply(want, 0x82, "1.2.3.4"));

	/* A request while the last reply is still going out is dropped. */
	ready();
	lay_out_image(1, 2, 3, 4);
	tbfake.busy_n = 1000;
	{
		const size_t n = smp_packet(pkt, OP_READ, 0, 1, 0x83, 0, &b);

		(void)write_in(TB_DFU_H_SMP, pkt, n, n);
		(void)tbfake_advance_ms(5);
		pkt[6] = 0x84;
		(void)write_in(TB_DFU_H_SMP, pkt, n, n);
		tbfake.busy_n = 0;
		(void)tbfake_advance_ms(50);
		got = tbfake_stream(TB_DFU_H_SMP, rsp, sizeof(rsp));
		T_EQ("one reply", (long)got, (long)state_reply(want, 0x83, "1.2.3.4"));
		T_EQ("to the first request", (long)rsp[6], 0x83L);
	}

	/* The client turns notifications off mid-reply: the rest is dropped. */
	ready();
	lay_out_image(1, 2, 3, 4);
	tbfake.busy_n = 1000;
	{
		const size_t n = smp_packet(pkt, OP_READ, 0, 1, 0x85, 0, &b);

		(void)write_in(TB_DFU_H_SMP, pkt, n, n);
		tb_zephyr_lock();
		tb_dfu_on_ccc(TB_DFU_H_SMP, 0);
		tb_zephyr_unlock();
		tbfake.busy_n = 0;
		(void)tbfake_advance_ms(50);
		T_EQ("nothing after the CCC write", (long)tbfake.notify_count, 0L);
	}

	/* A partial request, then the link drops: the next one parses clean. */
	ready();
	lay_out_image(1, 2, 3, 4);
	{
		const size_t n = smp_packet(pkt, OP_READ, 0, 1, 0x86, 0, &b);

		(void)write_in(TB_DFU_H_SMP, pkt, 5, 5);
		tb_zephyr_lock();
		tb_dfu_on_disconnect(0x08);
		tb_zephyr_unlock();
		got = call(OP_READ, 0, 1, 0x87, 0, &b);
		T_EQ("fresh request after a disconnect", (long)got,
		     (long)state_reply(want, 0x87, "1.2.3.4"));
		(void)n;
	}

	/* Not one of the two values. */
	T_EQ("other handle: Invalid Handle", (long)write_in(0x0020, pkt, 4, 4), 0x01L);
}

/* ---- the native frames --------------------------------------------------------- */

static size_t frame(uint8_t *out, uint8_t op, uint32_t id, uint32_t a, const uint8_t *data,
		    size_t len, bool with_a)
{
	size_t n = 0;

	out[n++] = op;
	tinyfota_put_le32(id, &out[n]);
	n += 4;
	if (with_a) {
		tinyfota_put_le32(a, &out[n]);
		n += 4;
	}
	if (len > 0u) {
		memcpy(&out[n], data, len);
	}
	return n + len;
}

/** Write one native frame, run the queue, return the reply. */
static size_t native(const uint8_t *f, size_t n)
{
	tbfake_clear_notify();
	(void)write_in(TB_DFU_H_NATIVE, f, n, n);
	(void)tbfake_advance_ms(10);
	return tbfake_stream(TB_DFU_H_NATIVE, rsp, sizeof(rsp));
}

static bool rsp_ok(size_t got, uint32_t id, uint32_t next)
{
	return got == 9 && rsp[0] == TINYFOTA_RSP_OK && tinyfota_get_le32(&rsp[1]) == id &&
	       tinyfota_get_le32(&rsp[5]) == next;
}

static void test_native(void)
{
	const uint32_t id = 0x1234u;
	uint8_t f[256];
	size_t got;
	size_t off = 0;
	bool ok = true;
	unsigned reboots;

	t_group("tinyble native frames");
	build_wire();

	/* Closed: refused inside the handler, as on the Zephyr host's GATT. */
	ready();
	got = native(f, frame(f, TINYFOTA_OP_BEGIN, id, sizeof(s_wire), NULL, 0, true));
	T_OK("closed window: ERR CLOSED",
	     got == 2 && rsp[0] == TINYFOTA_RSP_ERR && rsp[1] == TINYFOTA_ERR_CLOSED);

	/* A whole transfer, DATA sized to a 247 B MTU. */
	ready();
	reboots = tinyfota_flash_host_reboots();
	tinyfota_window_open(100000);
	tbfake.mtu = 247;
	got = native(f, frame(f, TINYFOTA_OP_BEGIN, id, sizeof(s_wire), NULL, 0, true));
	T_OK("BEGIN ok, next 0", rsp_ok(got, id, 0));
	while (off < sizeof(s_wire)) {
		const size_t room = 247u - 3u - 9u;
		const size_t n = (sizeof(s_wire) - off < room) ? sizeof(s_wire) - off : room;

		got = native(f, frame(f, TINYFOTA_OP_DATA, id, (uint32_t)off, s_wire + off, n, true));
		off += n;
		ok = ok && rsp_ok(got, id, (uint32_t)off);
	}
	T_OK("every DATA answered with the next offset", ok);
	got = native(f, frame(f, TINYFOTA_OP_COMMIT, id, 0, NULL, 0, false));
	T_OK("COMMIT ok", rsp_ok(got, id, sizeof(s_wire)));
	T_OK("staged", tinyfota_rx_staged());
	(void)tinyfota_osal_host_advance_ms(600);
	T_EQ("the receiver rebooted after its reply", (long)tinyfota_flash_host_reboots(),
	     (long)reboots + 1);

	/* A resend from a stale offset is answered with the real one. */
	ready();
	tinyfota_window_open(100000);
	(void)native(f, frame(f, TINYFOTA_OP_BEGIN, id, sizeof(s_wire), NULL, 0, true));
	(void)native(f, frame(f, TINYFOTA_OP_DATA, id, 0, s_wire, 100, true));
	got = native(f, frame(f, TINYFOTA_OP_DATA, id, 40, s_wire + 40, 60, true));
	T_OK("stale offset: next offset 100", rsp_ok(got, id, 100));

	/* SMP cannot take the receiver from the native owner mid-transfer. */
	{
		struct cb b;

		upload_body(&b, 0, 50);
		got = call(OP_WRITE, 0, 1, 0x90, 1, &b);
		expect_error("SMP refused while native holds the claim", got, OP_WRITE_RSP, 0, 1, 0x90,
			     1, 6);
	}

	/* The link drops: the claim goes with it. */
	tb_zephyr_lock();
	tb_dfu_on_disconnect(0x08);
	tb_zephyr_unlock();
	got = native(f, frame(f, TINYFOTA_OP_DATA, id, 100, s_wire + 100, 50, true));
	T_OK("after a disconnect: ERR SEQUENCE",
	     got == 2 && rsp[0] == TINYFOTA_RSP_ERR && rsp[1] == TINYFOTA_ERR_SEQUENCE);

	/* TB_EBUSY on the native value: retried. */
	ready();
	tinyfota_window_open(100000);
	tbfake.busy_n = 3;
	got = native(f, frame(f, TINYFOTA_OP_BEGIN, id, sizeof(s_wire), NULL, 0, true));
	T_OK("reply survives TB_EBUSY", rsp_ok(got, id, 0));
	T_EQ("native retries under the lock", (long)tbfake.unlocked_calls, 0L);

	/* An empty write is malformed, not a crash. */
	got = native(f, 0);
	T_OK("empty frame: ERR MALFORMED",
	     got == 2 && rsp[0] == TINYFOTA_RSP_ERR && rsp[1] == TINYFOTA_ERR_MALFORMED);
}

int main(void)
{
	test_state();
	test_upload();
	test_malformed();
	test_os();
	test_reset();
	test_notify();
	test_native();

	if (t_fail > 0) {
		printf("  tinyble: FAIL (%d of %d)\n", t_fail, t_fail + t_pass);
		return 1;
	}
	printf("  tinyble: PASS (%d checks — SMP and native frames over real zcbor)\n", t_pass);
	return 0;
}
