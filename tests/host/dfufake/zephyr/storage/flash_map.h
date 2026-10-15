/* dfufake: <zephyr/storage/flash_map.h>, only the macro the host suites reach
 * through tinyfota/tinyfota_partitions.h.
 *
 * dfu_smp_img.c reads the running image's MCUboot header straight through
 * PARTITION_ADDRESS(slot0_partition). Pointing it at a RAM buffer the suite
 * fills (declared in dfufake.h) is what lets the version and hash it reports
 * be checked without an image in flash. */
#ifndef DFUFAKE_FLASH_MAP_H
#define DFUFAKE_FLASH_MAP_H

#include "dfufake.h"

#define PARTITION_ADDRESS(label) ((uintptr_t)dfufake_running_image)

#endif /* DFUFAKE_FLASH_MAP_H */
