/* SPDX-License-Identifier: Apache-2.0 */

/*
 * flash_zephyr.c - the Zephyr backend of the flash seams in tinyfota_port.h: flash_map areas named
 * by partition label (tinyfota/tinyfota_partitions.h), so the mapping is decided
 * by the image that compiles this file (app and MCUboot both build tinyfota,
 * each against its own partition map). Alignment is the driver's to enforce
 * here; the host backend enforces the same rules deliberately.
 */
#if defined(__ZEPHYR__)

#include <errno.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "tinyfota.h"
#include "tinyfota/tinyfota_partitions.h"
#include "tinyfota/tinyfota_port.h"

static uint8_t pm_id_of(enum tinyfota_flash_area_id id)
{
	return id == TINYFOTA_FLASH_AREA_PRIMARY ? TINYFOTA_PARTITION_ID(slot0_partition)
						 : TINYFOTA_PARTITION_ID(patch_staging);
}

/*
 * The geometry seam. The staging format is laid out in TINYFOTA_PAGE_SIZE pages
 * and the core writes in 4 B words (tinyfota.h, tinyfota_port.h). A part whose
 * erase page is larger would erase the header page along with the step log,
 * and one whose write block is larger would refuse the core's writes. Both
 * fail late and confusingly, so an area that does not fit is refused at open.
 */
static int geometry_ok(const struct flash_area *fa)
{
	const struct device *dev = flash_area_get_device(fa);
	size_t wbs;

	if (dev == NULL) {
		return -ENODEV;
	}
	wbs = flash_get_write_block_size(dev);
	if (wbs == 0U || (4U % wbs) != 0U) {
		return -ENOTSUP;
	}
	if ((fa->fa_off % TINYFOTA_PAGE_SIZE) != 0U || (fa->fa_size % TINYFOTA_PAGE_SIZE) != 0U) {
		return -ENOTSUP;
	}
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	{
		struct flash_pages_info first;
		struct flash_pages_info last;

		/* First and last page: uniform pages are a requirement, not
		 * something this proves for every page in between. */
		if (flash_get_page_info_by_offs(dev, fa->fa_off, &first) != 0 ||
		    flash_get_page_info_by_offs(dev, fa->fa_off + fa->fa_size - 1U, &last) != 0) {
			return -EIO;
		}
		if (first.size != last.size || first.size == 0U ||
		    (TINYFOTA_PAGE_SIZE % first.size) != 0U) {
			return -ENOTSUP;
		}
	}
#endif
	return 0;
}

int tinyfota_flash_open(enum tinyfota_flash_area_id id,
			     const struct tinyfota_flash_area **fa)
{
	const struct flash_area *area;
	int rc = flash_area_open(pm_id_of(id), &area);

	if (rc != 0) {
		return rc;
	}
	rc = geometry_ok(area);
	if (rc != 0) {
		flash_area_close(area);
		return rc;
	}
	*fa = (const struct tinyfota_flash_area *)area;
	return 0;
}

void tinyfota_flash_close(const struct tinyfota_flash_area *fa)
{
	flash_area_close((const struct flash_area *)fa);
}

size_t tinyfota_flash_size(const struct tinyfota_flash_area *fa)
{
	return ((const struct flash_area *)fa)->fa_size;
}

int tinyfota_flash_read(const struct tinyfota_flash_area *fa, uint32_t off, void *dst,
			     size_t len)
{
	return flash_area_read((const struct flash_area *)fa, off, dst, len);
}

int tinyfota_flash_write(const struct tinyfota_flash_area *fa, uint32_t off,
			      const void *src, size_t len)
{
	return flash_area_write((const struct flash_area *)fa, off, src, len);
}

int tinyfota_flash_erase(const struct tinyfota_flash_area *fa, uint32_t off, size_t len)
{
	return flash_area_erase((const struct flash_area *)fa, off, len);
}

void tinyfota_reboot(void)
{
	sys_reboot(SYS_REBOOT_COLD);
}

#endif /* __ZEPHYR__ */
