/**
 * @file test_dfu_smp.c — the SMP image-management group on host.
 *
 * File under test: ports/zephyr/dfu/dfu_smp_img.c
 *
 * This is the second front door onto the same receiver: a stock mcumgr client
 * (nRF Device Manager, `mcumgr image upload`) pushes CBOR at group 1 and every
 * byte still goes through tinyfota_rx_upload(). So the checks here are about
 * the adapter and nothing else — which keys it emits, which mgmt error it
 * returns for each refusal, and that the window gate stands in front of both
 * upload and erase.
 *
 * The suite reaches the handlers only through the group the module registers,
 * so a wrong group id, a wrong command slot or a read handler mistakenly wired
 * to a write command fails here rather than on a phone. See tests/host/smpfake
 * for what the zcbor and mcumgr doubles do and do not prove.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dfufake.h"
#include "psafake.h"
#include "smpfake.h"
#include "test.h"

#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/util.h>

#include "dfu_crc.h"

#include "tinyfota.h"
#include "tinyfota_rx.h"

/* dfu_smp_img.c's MCUMGR_HANDLER_DEFINE, made callable by smpfake. */
extern void (*const smpfake_handler_tinyfota_smp_img)(void);

#define TINYFOTA_SMP_GRP_IMG       1
#define TINYFOTA_SMP_IMG_ID_STATE  0
#define TINYFOTA_SMP_IMG_ID_UPLOAD 1
#define TINYFOTA_SMP_IMG_ID_ERASE  5

#define IMAGE_MAGIC               0x96f3b83du
#define IMAGE_TLV_INFO_MAGIC      0x6907
#define IMAGE_TLV_PROT_INFO_MAGIC 0x6908
#define IMAGE_TLV_SHA256          0x10

#define HEAD_LEN (TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN)

static struct smpfake_nb reader_nb;
static struct smpfake_nb writer_nb;
static struct smp_streamer streamer;

static const struct mgmt_group *img_group;
static const struct mgmt_callback *reset_cb;

/* Registration happens once, the way mcumgr does it at boot. What it produced
 * is kept here rather than read back out of smpfake later, because every
 * ready() clears the recorder. */
static void register_group(void)
{
	smpfake_handler_tinyfota_smp_img();
	img_group = smpfake.registered_group;
	reset_cb = smpfake.registered_callback;
}

static struct smp_streamer *ctxt(void)
{
	streamer.reader = &reader_nb;
	streamer.writer = &writer_nb;
	return &streamer;
}

/** Reset every double; the group registration is process-wide and stays. */
static void ready(void)
{
	dfufake_reset();
	psafake_reset();
	smpfake_reset();
	tinyfota_rx_reset_all();
	tinyfota_window_close();
}

/* ---- the running image's identity ----------------------------------------- */

/**
 * Lay an MCUboot image out at PM_MCUBOOT_PAD_ADDRESS: header, a body of the
 * declared size, then a TLV block. @p prot_first prepends a protected TLV
 * block, which the reader has to step over to find the real one.
 */
static void build_running_image(uint8_t major, uint8_t minor, uint16_t rev, uint32_t build,
				uint16_t tlv_magic, const uint8_t *sha, uint16_t sha_len,
				bool prot_first)
{
	uint8_t *p = dfufake_running_image;
	const uint16_t hdr_size = 32;
	const uint32_t img_size = 64;
	uint8_t *tlv;
	uint16_t total;

	memset(dfufake_running_image, 0, DFUFAKE_PAGE_SIZE);

	/* struct image_header, little-endian, as MCUboot writes it. */
	p[0] = (uint8_t)IMAGE_MAGIC;
	p[1] = (uint8_t)(IMAGE_MAGIC >> 8);
	p[2] = (uint8_t)(IMAGE_MAGIC >> 16);
	p[3] = (uint8_t)(IMAGE_MAGIC >> 24);
	p[8] = (uint8_t)hdr_size;
	p[9] = (uint8_t)(hdr_size >> 8);
	p[12] = (uint8_t)img_size;
	p[13] = (uint8_t)(img_size >> 8);
	p[20] = major;
	p[21] = minor;
	p[22] = (uint8_t)rev;
	p[23] = (uint8_t)(rev >> 8);
	p[24] = (uint8_t)build;
	p[25] = (uint8_t)(build >> 8);
	p[26] = (uint8_t)(build >> 16);
	p[27] = (uint8_t)(build >> 24);

	tlv = p + hdr_size + img_size;
	if (prot_first) {
		/* A protected block carries its own info header and comes
		 * first; its it_tlv_tot covers itself. */
		const uint16_t prot_total = 8;

		tlv[0] = (uint8_t)IMAGE_TLV_PROT_INFO_MAGIC;
		tlv[1] = (uint8_t)(IMAGE_TLV_PROT_INFO_MAGIC >> 8);
		tlv[2] = (uint8_t)prot_total;
		tlv[3] = (uint8_t)(prot_total >> 8);
		tlv += prot_total;
	}

	total = (uint16_t)(4 + 4 + sha_len);
	tlv[0] = (uint8_t)tlv_magic;
	tlv[1] = (uint8_t)(tlv_magic >> 8);
	tlv[2] = (uint8_t)total;
	tlv[3] = (uint8_t)(total >> 8);
	tlv[4] = IMAGE_TLV_SHA256;
	tlv[5] = 0;
	tlv[6] = (uint8_t)sha_len;
	tlv[7] = (uint8_t)(sha_len >> 8);
	if (sha != NULL && sha_len > 0U) {
		memcpy(tlv + 8, sha, sha_len);
	}
}

/** The slot map has one entry per key; find a key and read the item after it. */
static const struct smpfake_item *value_after(const char *key)
{
	const int index = smpfake_find(key);

	return (index < 0) ? NULL : smpfake_item(index + 1);
}

static void test_registration(void)
{
	t_group("dfu smp registration");

	smpfake_reset();
	register_group();

	T_EQ("one group registered", (long)smpfake.register_group_calls, 1L);
	T_OK("group recorded", img_group != NULL);
	if (img_group == NULL) {
		return;
	}
	T_EQ("claims the image group id", (long)img_group->mg_group_id, (long)TINYFOTA_SMP_GRP_IMG);
	T_EQ("serves six command slots", (long)img_group->mg_handlers_count, 6L);

	/* State is the only command a client may read; upload and erase are
	 * write-only, and the three in between must stay unsupported. */
	T_OK("state read served", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_read != NULL);
	T_OK("state write served",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_write != NULL);
	T_OK("upload has no read",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_UPLOAD].mh_read == NULL);
	T_OK("upload write served",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_UPLOAD].mh_write != NULL);
	T_OK("erase has no read", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_ERASE].mh_read == NULL);
	T_OK("erase write served",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_ERASE].mh_write != NULL);
	for (int id = 2; id <= 4; id++) {
		T_OK("file/corelist/coreload unsupported",
		     img_group->mg_handlers[id].mh_read == NULL &&
			     img_group->mg_handlers[id].mh_write == NULL);
	}

	/* The reset veto is registered for exactly the reset event. */
	T_EQ("one callback registered", (long)smpfake.register_callback_calls, 1L);
	T_OK("callback recorded", smpfake.registered_callback != NULL);
	if (smpfake.registered_callback != NULL) {
		const struct mgmt_callback *cb = smpfake.registered_callback;

		T_EQ("hooked on os reset", (long)cb->event_id, (long)MGMT_EVT_OP_OS_MGMT_RESET);
		T_OK("hook has a function", cb->callback != NULL);
	}
}

static void test_state_read(void)
{
	uint8_t sha[32];
	const struct smpfake_item *item;

	t_group("dfu smp image state");

	for (size_t i = 0; i < sizeof(sha); i++) {
		sha[i] = (uint8_t)(0xc0 + i);
	}

	/* A valid image: the version and the hash the bootloader recorded. */
	ready();
	build_running_image(1, 2, 3, 4, IMAGE_TLV_INFO_MAGIC, sha, sizeof(sha), false);
	T_EQ("state read ok", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_read(ctxt()),
	     MGMT_ERR_EOK);
	T_OK("images key present", smpfake_find("images") >= 0);
	T_OK("splitStatus key present", smpfake_find("splitStatus") >= 0);
	item = value_after("version");
	T_OK("version encoded", item != NULL && item->kind == SMPFAKE_TSTR);
	if (item != NULL) {
		T_OK("version from the running header", strcmp(item->text, "1.2.3.4") == 0);
	}
	item = value_after("hash");
	T_OK("hash encoded", item != NULL && item->kind == SMPFAKE_BSTR && item->len == 32u);
	if (item != NULL) {
		T_OK("hash is the recorded sha256", memcmp(item->bytes, sha, sizeof(sha)) == 0);
	}
	/* One slot, and it is the one that is running: a second would need an
	 * honest hash for bytes that are still a patch. */
	item = value_after("slot");
	T_OK("single slot 0", item != NULL && item->value == 0);
	item = value_after("pending");
	T_OK("nothing pending", item != NULL && item->kind == SMPFAKE_BOOL && item->value == 0);
	item = value_after("confirmed");
	T_OK("confirmed", item != NULL && item->value == 1);
	item = value_after("active");
	T_OK("active", item != NULL && item->value == 1);

	/* A protected TLV block comes first and has to be stepped over. */
	ready();
	build_running_image(9, 8, 7, 6, IMAGE_TLV_INFO_MAGIC, sha, sizeof(sha), true);
	T_EQ("state read ok", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_read(ctxt()),
	     MGMT_ERR_EOK);
	item = value_after("version");
	T_OK("version past the protected block",
	     item != NULL && strcmp(item->text, "9.8.7.6") == 0);
	item = value_after("hash");
	T_OK("hash found past the protected block",
	     item != NULL && memcmp(item->bytes, sha, sizeof(sha)) == 0);

	/* No image at all: reported as a zero version and a zero hash rather
	 * than as a refusal, which is what a client can parse. */
	ready();
	memset(dfufake_running_image, 0, DFUFAKE_PAGE_SIZE);
	T_EQ("state read ok", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_read(ctxt()),
	     MGMT_ERR_EOK);
	item = value_after("version");
	T_OK("zero version", item != NULL && strcmp(item->text, "0.0.0.0") == 0);
	item = value_after("hash");
	T_OK("zero hash", item != NULL && item->len == 32u);
	if (item != NULL) {
		uint8_t zeros[32] = {0};

		T_OK("hash is all zeros", memcmp(item->bytes, zeros, sizeof(zeros)) == 0);
	}

	/* A TLV block that is not where the header says: hash reported as
	 * zeros, version still read from the header. */
	ready();
	build_running_image(1, 0, 0, 0, 0x1234, sha, sizeof(sha), false);
	T_EQ("state read ok", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_read(ctxt()),
	     MGMT_ERR_EOK);
	item = value_after("hash");
	if (item != NULL) {
		uint8_t zeros[32] = {0};

		T_OK("unfindable hash reported as zeros",
		     memcmp(item->bytes, zeros, sizeof(zeros)) == 0);
	}

	/* A SHA TLV of the wrong length is not the hash; the walk runs off the
	 * end of the block and reports zeros. */
	ready();
	build_running_image(1, 0, 0, 0, IMAGE_TLV_INFO_MAGIC, sha, 16, false);
	T_EQ("state read ok", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_read(ctxt()),
	     MGMT_ERR_EOK);
	item = value_after("hash");
	if (item != NULL) {
		uint8_t zeros[32] = {0};

		T_OK("short sha tlv reported as zeros",
		     memcmp(item->bytes, zeros, sizeof(zeros)) == 0);
	}

	/* A response that will not fit is a message-size error, not a lie. */
	ready();
	build_running_image(1, 2, 3, 4, IMAGE_TLV_INFO_MAGIC, sha, sizeof(sha), false);
	smpfake.encode_fail_in = 0;
	T_EQ("full buffer reported",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_read(ctxt()),
	     MGMT_ERR_EMSGSIZE);
	ready();
	build_running_image(1, 2, 3, 4, IMAGE_TLV_INFO_MAGIC, sha, sizeof(sha), false);
	smpfake.encode_fail_in = 5; /* fails part-way through the slot map */
	T_EQ("partial encode reported",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_read(ctxt()), MGMT_ERR_EMSGSIZE);
}

static void test_state_write(void)
{
	uint8_t hash[32] = {0xaa};
	const struct smpfake_kv request[] = {
		{"hash", SMPFAKE_BSTR, 0, hash, sizeof(hash)},
		{"confirm", SMPFAKE_BOOL, 1, NULL, 0},
	};

	t_group("dfu smp image state write");

	/* Marking an image test-or-confirm changes nothing here: an update is
	 * either fully staged or it is not. The reply is the current list,
	 * which is both truthful and what the client parses. */
	ready();
	build_running_image(2, 0, 1, 0, IMAGE_TLV_INFO_MAGIC, hash, sizeof(hash), false);
	smpfake_request(request, ARRAY_SIZE(request));
	T_EQ("state write ok", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_STATE].mh_write(ctxt()),
	     MGMT_ERR_EOK);
	T_EQ("request decoded", (long)smpfake.decode_bulk_calls, 1L);
	T_EQ("both fields matched", (long)smpfake.decoded_keys, 2L);
	T_OK("answered with the image list", smpfake_find("images") >= 0);
	T_OK("nothing was marked", !tinyfota_rx_staged());
}

/* ---- upload ---------------------------------------------------------------- */

static uint8_t upload_head[HEAD_LEN];
static uint8_t upload_patch[48];
static uint8_t upload_wire[HEAD_LEN + sizeof(upload_patch)];

static void build_upload_wire(void)
{
	struct tinyfota_hdr hdr;

	for (size_t i = 0; i < sizeof(upload_patch); i++) {
		upload_patch[i] = (uint8_t)(0x30 + i * 3u);
	}
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = TINYFOTA_MAGIC;
	hdr.abi_version = TINYFOTA_ABI_VERSION;
	hdr.patch_len = sizeof(upload_patch);
	hdr.to_len = 0;
	hdr.patch_crc32 = tinyfota_crc32(upload_patch, sizeof(upload_patch));
	hdr.from_crc32 = 0;
	hdr.from_len = 0;
	memcpy(upload_head, &hdr, sizeof(hdr));
	hdr.hdr_crc32 = tinyfota_crc32(upload_head, TINYFOTA_HDR_CRC_LEN);
	memcpy(upload_head, &hdr, sizeof(hdr));
	memset(upload_head + TINYFOTA_HDR_LEN, 0xa5, TINYFOTA_SIG_LEN);

	memcpy(upload_wire, upload_head, HEAD_LEN);
	memcpy(upload_wire + HEAD_LEN, upload_patch, sizeof(upload_patch));
}

static int upload(uint32_t image, size_t off, size_t total, const uint8_t *data, size_t len,
		  bool with_off)
{
	struct smpfake_kv request[6];
	size_t n = 0;

	request[n++] = (struct smpfake_kv){"image", SMPFAKE_UINT, image, NULL, 0};
	request[n++] = (struct smpfake_kv){"data", SMPFAKE_BSTR, 0, data, len};
	request[n++] = (struct smpfake_kv){"len", SMPFAKE_UINT, total, NULL, 0};
	if (with_off) {
		request[n++] = (struct smpfake_kv){"off", SMPFAKE_UINT, off, NULL, 0};
	}
	smpfake_request(request, n);
	return img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_UPLOAD].mh_write(ctxt());
}

static void test_upload(void)
{
	const struct smpfake_item *item;
	const size_t total = sizeof(upload_wire);

	t_group("dfu smp upload");

	build_upload_wire();

	/* The window is shut: the one refusal a legitimate owner will meet, and
	 * it gets its own code so the client says "access denied" rather than
	 * something the owner cannot act on. */
	ready();
	T_EQ("upload refused while closed", upload(0, 0, total, upload_wire, total, true),
	     MGMT_ERR_EACCESSDENIED);

	/* A request that will not decode at all. */
	ready();
	tinyfota_window_open(1000);
	smpfake.decode_bulk_ret = -1;
	T_EQ("undecodable request refused",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_UPLOAD].mh_write(ctxt()), MGMT_ERR_EINVAL);

	/* mcumgr's offset is mandatory; without it there is no way to know what
	 * these bytes are. */
	ready();
	tinyfota_window_open(1000);
	T_EQ("missing offset refused", upload(0, 0, total, upload_wire, total, false),
	     MGMT_ERR_EINVAL);

	/* Only image 0 exists; writing another image's bytes into the one slot
	 * there is would be worse than saying so. */
	ready();
	tinyfota_window_open(1000);
	T_EQ("second image refused", upload(1, 0, total, upload_wire, total, true),
	     MGMT_ERR_EINVAL);

	/* The whole file in one chunk. */
	ready();
	tinyfota_window_open(1000);
	T_EQ("upload accepted", upload(0, 0, total, upload_wire, total, true), MGMT_ERR_EOK);
	item = value_after("off");
	T_OK("next offset echoed", item != NULL && item->kind == SMPFAKE_UINT);
	if (item != NULL) {
		T_EQ("next offset is the whole file", (long)item->value, (long)total);
	}
	T_OK("staged", tinyfota_rx_staged());
	/* Legacy clients want an explicit rc alongside the offset. */
	T_OK("legacy rc emitted", smpfake_find("rc") >= 0);

	/* Chunked, with the device reporting its position each time. */
	ready();
	tinyfota_window_open(1000);
	{
		size_t off = 0;
		const size_t chunk = 24;

		while (off < total) {
			const size_t n = (total - off < chunk) ? total - off : chunk;

			smpfake_reset();
			T_EQ("chunk accepted", upload(0, off, total, upload_wire + off, n, true),
			     MGMT_ERR_EOK);
			off += n;
			item = value_after("off");
			T_OK("position echoed", item != NULL && (size_t)item->value == off);
		}
		T_OK("staged after chunking", tinyfota_rx_staged());
	}

	/* A stale offset is a resync, which the protocol treats as success. */
	ready();
	tinyfota_window_open(1000);
	T_EQ("first chunk", upload(0, 0, total, upload_wire, 24, true), MGMT_ERR_EOK);
	smpfake_reset();
	T_EQ("stale offset accepted", upload(0, 999, total, upload_wire, 24, true), MGMT_ERR_EOK);
	item = value_after("off");
	T_OK("resync reports the real position", item != NULL && item->value == 24);

	/* A refusal from the receiver is a bad-state error: the transfer is
	 * discarded and the client must restart at 0. */
	ready();
	tinyfota_window_open(1000);
	psafake.verify_ret = -1;
	T_EQ("bad signature refused", upload(0, 0, total, upload_wire, total, true),
	     MGMT_ERR_EBADSTATE);
	T_OK("nothing staged", !tinyfota_rx_staged());

	/* A reply that will not fit. */
	ready();
	tinyfota_window_open(1000);
	smpfake.encode_fail_in = 0;
	T_EQ("full reply buffer reported", upload(0, 0, total, upload_wire, total, true),
	     MGMT_ERR_EMSGSIZE);
}

static void test_erase(void)
{
	t_group("dfu smp erase");

	/* Erasing is gated on the window for the same reason uploading is: an
	 * unauthenticated peer that can erase in a loop is an availability
	 * attack on a door lock. */
	ready();
	T_EQ("erase refused while closed",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_ERASE].mh_write(ctxt()),
	     MGMT_ERR_EACCESSDENIED);
	T_EQ("nothing erased", (long)dfufake_staging.erase_calls, 0L);

	/* A staged update is thrown away, and the receiver forgets it too. */
	ready();
	tinyfota_window_open(1000);
	build_upload_wire();
	T_EQ("stage something first",
	     upload(0, 0, sizeof(upload_wire), upload_wire, sizeof(upload_wire), true),
	     MGMT_ERR_EOK);
	T_OK("staged", tinyfota_rx_staged());
	T_EQ("erase ok", img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_ERASE].mh_write(ctxt()),
	     MGMT_ERR_EOK);
	T_EQ("erased the whole partition", (long)dfufake_staging.last_erase_len,
	     (long)DFUFAKE_STAGING_SIZE);
	T_OK("no longer staged", !tinyfota_rx_staged());

	/* Flash failures are reported rather than swallowed. */
	ready();
	tinyfota_window_open(1000);
	dfufake_staging.erase_fail_in = 0;
	T_EQ("failed erase reported",
	     img_group->mg_handlers[TINYFOTA_SMP_IMG_ID_ERASE].mh_write(ctxt()),
	     MGMT_ERR_EUNKNOWN);
}

static void test_reset_gate(void)
{
	const struct mgmt_callback *cb = reset_cb;
	int32_t rc;
	uint16_t group = 0;
	bool abort_more = false;

	t_group("dfu smp reset gate");

	if (cb == NULL || cb->callback == NULL) {
		T_OK("reset hook registered", 0);
		return;
	}

	/* Anything that is not the reset event passes straight through. */
	ready();
	rc = 0;
	T_EQ("unrelated event ignored",
	     cb->callback(MGMT_EVT_OP_IMG_MGMT_DFU_STARTED, MGMT_CB_OK, &rc, &group, &abort_more,
			  NULL, 0),
	     MGMT_CB_OK);
	T_EQ("no error code written", (long)rc, 0L);

	/* THE CASE THIS HOOK EXISTS FOR. With the SMP endpoint writable by any
	 * unpaired peer in radio range, group 0 command 5 would let anyone hold
	 * the lock in a reboot loop. Outside an update it must be refused. */
	ready();
	rc = 0;
	T_EQ("reset refused when idle",
	     cb->callback(MGMT_EVT_OP_OS_MGMT_RESET, MGMT_CB_OK, &rc, &group, &abort_more, NULL, 0),
	     MGMT_CB_ERROR_RC);
	T_EQ("refusal is access denied", (long)rc, (long)MGMT_ERR_EACCESSDENIED);

	/* An owner who opened a window is allowed to reboot. */
	ready();
	tinyfota_window_open(1000);
	rc = 0;
	T_EQ("reset allowed with the window open",
	     cb->callback(MGMT_EVT_OP_OS_MGMT_RESET, MGMT_CB_OK, &rc, &group, &abort_more, NULL, 0),
	     MGMT_CB_OK);
	T_EQ("no error code written", (long)rc, 0L);

	/* So is a reboot that installs an update already staged and verified,
	 * even though the window has since closed. */
	ready();
	tinyfota_window_open(1000);
	build_upload_wire();
	T_EQ("stage an update",
	     upload(0, 0, sizeof(upload_wire), upload_wire, sizeof(upload_wire), true),
	     MGMT_ERR_EOK);
	tinyfota_window_close();
	rc = 0;
	T_EQ("reset allowed with an update staged",
	     cb->callback(MGMT_EVT_OP_OS_MGMT_RESET, MGMT_CB_OK, &rc, &group, &abort_more, NULL, 0),
	     MGMT_CB_OK);
}

void test_dfu_smp(void)
{
	test_registration();
	if (img_group == NULL) {
		return;
	}
	test_state_read();
	test_state_write();
	test_upload();
	test_erase();
	test_reset_gate();
}
