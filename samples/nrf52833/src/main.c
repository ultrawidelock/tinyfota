/* SPDX-License-Identifier: ISC */

/*
 * tinyfota sample: the smallest application that takes a delta update.
 *
 * It advertises the two services an updater looks for, keeps a connection slot
 * free for one, opens an update window when sw0 is pressed, and says at every
 * boot which image it is running. Everything else is the tinyfota module:
 * ports/zephyr/dfu_ble_zephyr.c (L2CAP CoC, GATT, the button), and
 * ports/zephyr/dfu_smp_img.c (SMP over Bluetooth and uart0).
 *
 * No update is accepted until the window is open. The patch is signed and
 * MCUboot re-verifies the result, so the window is not what stops a forged
 * update; it is what stops a stranger in radio range from spending erase
 * cycles and forcing reboots.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <string.h>

#include "tinyfota_rx.h"
#include "tinyfota/tinyfota_partitions.h"

LOG_MODULE_REGISTER(sample, LOG_LEVEL_INF);

/*
 * RESERVE A CONNECTION FOR UPDATES. The controller stops advertising when a
 * peer connects, so a board with one slot that is in use is invisible to
 * every updater -- not slow to find, invisible: a browser's chooser stays
 * empty. Two slots, and advertising restarted while one is free, is the fix
 * (ultrawidelock commit 91bf7f0f). An application built on this sample keeps
 * one slot for its own peer and one for the updater.
 */
BUILD_ASSERT(CONFIG_BT_MAX_CONN >= 2,
	     "tinyfota: reserve a connection for updates -- set CONFIG_BT_MAX_CONN=2 or more");

/*
 * FOUND BY SERVICE UUID, never by name. A 31-byte advertisement truncates the
 * name, and a name filter then fails silently (ultrawidelock commit 6001dd51).
 * The SMP service goes in the advertisement, the native update service in the
 * scan response; every tinyfota host tool filters on one or the other.
 */
static const struct bt_data k_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_128_ENCODE(0x8d53dc1d, 0x1db7, 0x4cd3, 0x868b, 0x8a527460aa84)),
};

static const struct bt_data k_sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_128_ENCODE(0xd3b5a140, 0x9e23, 0x4b3a, 0x8be4, 0x6b1ee5f980a3)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static atomic_t s_conn_count;

static void adv_work_fn(struct k_work *work)
{
	int rc;

	ARG_UNUSED(work);
	if (atomic_get(&s_conn_count) >= CONFIG_BT_MAX_CONN) {
		return; /* a connectable advertisement with no free slot would be a lie */
	}
	rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, k_ad, ARRAY_SIZE(k_ad), k_sd,
			     ARRAY_SIZE(k_sd));
	if (rc != 0 && rc != -EALREADY) {
		LOG_WRN("advertising rc=%d", rc);
	}
}
static K_WORK_DEFINE(s_adv_work, adv_work_fn);

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);
	if (err == 0U) {
		atomic_inc(&s_conn_count);
	}
	/* The controller ended the advertising set; restart it while a slot
	 * is still free, or the second slot is unreachable. */
	(void)k_work_submit(&s_adv_work);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(reason);
	/* Clamped at zero: a failed establishment can be reported here without a
	 * matching connected count, and a negative count would read as "slots
	 * free" forever. */
	if (atomic_get(&s_conn_count) > 0) {
		atomic_dec(&s_conn_count);
	}
}

static void on_recycled(void)
{
	(void)k_work_submit(&s_adv_work);
}

BT_CONN_CB_DEFINE(s_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = on_recycled,
};

/* ---- the window indicator ------------------------------------------------ */

/* There is no way to see from outside the board that the window is open, so
 * led0 shows it. */
static const struct gpio_dt_spec s_led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

static void window_changed(bool open)
{
	if (s_led.port != NULL) {
		(void)gpio_pin_set_dt(&s_led, open ? 1 : 0);
	}
	LOG_INF("update window %s", open ? "open" : "closed");
}

/* ---- which image is this -------------------------------------------------- */

/*
 * Single-slot MCUboot has no test state for an image to confirm itself out of:
 * MCUboot validated this image before jumping to it, and the applier consumed
 * the patch that produced it. What the image owes the host is its identity, so
 * the host can confirm the update it pushed is the one that runs. The SMP
 * image list reports the same SHA-256 (ports/zephyr/dfu_smp_img.c).
 */
static void report_running_image(void)
{
	const uint8_t *base = (const uint8_t *)TINYFOTA_PARTITION_ADDRESS(slot0_partition);
	const uint8_t *tlv;
	const uint8_t *end;
	uint32_t magic, img_size;
	uint16_t hdr_size, tot;

	memcpy(&magic, base, sizeof(magic));
	if (magic != 0x96f3b83du) {
		LOG_WRN("no MCUboot header at the start of the slot");
		return;
	}
	memcpy(&hdr_size, base + 8, sizeof(hdr_size));
	memcpy(&img_size, base + 12, sizeof(img_size));
	tlv = base + hdr_size + img_size;
	memcpy(&tot, tlv + 2, sizeof(tot));
	if (tlv[0] == 0x08 && tlv[1] == 0x69) { /* protected TLVs first */
		tlv += tot;
		memcpy(&tot, tlv + 2, sizeof(tot));
	}
	end = tlv + tot;
	for (const uint8_t *p = tlv + 4; p + 4 <= end;) {
		uint16_t len;

		memcpy(&len, p + 2, sizeof(len));
		if (p[0] == 0x10 && len == 32U) {
			LOG_INF("running v%u.%u.%u+%u, sha256 %02x%02x%02x%02x%02x%02x%02x%02x...",
				base[20], base[21], (unsigned)(base[22] | (base[23] << 8)),
				(unsigned)(base[24] | (base[25] << 8) | (base[26] << 16) |
					   ((uint32_t)base[27] << 24)),
				p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
			return;
		}
		p += 4 + len;
	}
	LOG_WRN("running image has no SHA-256 TLV");
}

int main(void)
{
	int rc;

	report_running_image();

	if (s_led.port != NULL && gpio_is_ready_dt(&s_led)) {
		(void)gpio_pin_configure_dt(&s_led, GPIO_OUTPUT_INACTIVE);
	}
	tinyfota_set_window_cb(window_changed);

	rc = bt_enable(NULL);
	if (rc != 0) {
		LOG_ERR("bt_enable rc=%d", rc);
		return 0;
	}
	rc = tinyfota_ble_start();
	if (rc != 0) {
		LOG_ERR("update transport rc=%d", rc);
	}
	(void)k_work_submit(&s_adv_work);
	LOG_INF("ready: press sw0 to open a %u s update window",
		(unsigned)(CONFIG_TINYFOTA_WINDOW_MS / 1000));
	return 0;
}
