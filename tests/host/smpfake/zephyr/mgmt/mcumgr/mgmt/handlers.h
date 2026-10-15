/* smpfake: <zephyr/mgmt/mcumgr/mgmt/handlers.h>.
 *
 * On target MCUMGR_HANDLER_DEFINE() puts the init function in an iterable
 * section that mcumgr walks at boot. Here it emits a linkable pointer named
 * after the handler, the same trick logfake's SYS_INIT uses, so a suite can
 * run the registration and then assert on what it registered. */
#ifndef SMPFAKE_ZEPHYR_MGMT_MCUMGR_HANDLERS_H
#define SMPFAKE_ZEPHYR_MGMT_MCUMGR_HANDLERS_H

#define MCUMGR_HANDLER_DEFINE(name, fn) void (*const smpfake_handler_##name)(void) = (fn)

#endif /* SMPFAKE_ZEPHYR_MGMT_MCUMGR_HANDLERS_H */
