/* tbfake: <tinyble/tb_zephyr.h>, the lock a work item holds around tb_* calls.
 * The fake counts it, so a suite can check every tb_notify() ran under it. */
#ifndef TBFAKE_TB_ZEPHYR_H
#define TBFAKE_TB_ZEPHYR_H

void tb_zephyr_lock(void);
void tb_zephyr_unlock(void);

#endif /* TBFAKE_TB_ZEPHYR_H */
