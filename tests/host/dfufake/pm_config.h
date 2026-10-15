/* dfufake: <pm_config.h>, the partition manager's generated geometry.
 *
 * The sizes are the source project's DWM3001CDK partition map, so PATCH_MAX and the
 * "past slot" bound the code checks against are the numbers it will meet on a
 * DWM3001CDK rather than convenient round ones. The IDs are arbitrary but
 * distinct: nothing reads them except flash_area_open(), which looks them up. */
#ifndef DFUFAKE_PM_CONFIG_H
#define DFUFAKE_PM_CONFIG_H

#include "dfufake.h"

#define PM_PATCH_STAGING_ID   DFUFAKE_STAGING_ID
#define PM_PATCH_STAGING_SIZE DFUFAKE_STAGING_SIZE

#define PM_MCUBOOT_PRIMARY_ID   DFUFAKE_PRIMARY_ID
#define PM_MCUBOOT_PRIMARY_SIZE DFUFAKE_PRIMARY_SIZE

/* dfu_smp_img.c reads the running image's MCUboot header straight through this
 * address. Pointing it at a RAM buffer the suite fills (declared in dfufake.h)
 * is what lets the version and hash it reports be checked without an image in
 * flash. */
#define PM_MCUBOOT_PAD_ADDRESS ((uintptr_t)dfufake_running_image)

#endif /* DFUFAKE_PM_CONFIG_H */
