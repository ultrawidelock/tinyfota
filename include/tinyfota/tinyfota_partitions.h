/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * tinyfota_partitions.h - the two Zephyr partitions tinyfota uses, by
 * devicetree label, so one name works on upstream Zephyr and on NCS.
 *
 * slot0_partition is the one application slot, patch_staging the staging
 * partition. Upstream Zephyr resolves both labels through fixed-partitions
 * (samples/nrf52833/partitions.dtsi). NCS's Partition Manager maps the same
 * labels onto pm_static.yml: nrf/include/flash_map_pm.h turns slot0_partition
 * into mcuboot_primary, and patch_staging is a partition there by that name.
 * Zephyr v4.4 renamed FIXED_PARTITION_* to PARTITION_*; NCS v3.3.0 has only the
 * old names.
 */
#ifndef TINYFOTA_PARTITIONS_H
#define TINYFOTA_PARTITIONS_H

#include <zephyr/storage/flash_map.h>

#ifdef PARTITION_ADDRESS
#define TINYFOTA_PARTITION_ID(label)      PARTITION_ID(label)
#define TINYFOTA_PARTITION_ADDRESS(label) PARTITION_ADDRESS(label)
#else
#define TINYFOTA_PARTITION_ID(label)      FIXED_PARTITION_ID(label)
#define TINYFOTA_PARTITION_ADDRESS(label) FIXED_PARTITION_ADDRESS(label)
#endif

#endif /* TINYFOTA_PARTITIONS_H */
