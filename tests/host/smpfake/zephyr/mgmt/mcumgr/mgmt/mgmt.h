/* smpfake: <zephyr/mgmt/mcumgr/mgmt/mgmt.h> — the handler/group registry and
 * the error codes a handler returns. mgmt_register_group() records rather than
 * links, so a suite reaches the handlers exactly the way mcumgr would: through
 * the group the module registered. */
#ifndef SMPFAKE_ZEPHYR_MGMT_MCUMGR_MGMT_H
#define SMPFAKE_ZEPHYR_MGMT_MCUMGR_MGMT_H

#include <stddef.h>
#include <stdint.h>

/* Spellings and values are mcumgr's. A handler that returns the wrong one is
 * a different string on the client, so the numbers matter. */
#define MGMT_ERR_EOK            0
#define MGMT_ERR_EUNKNOWN       1
#define MGMT_ERR_ENOMEM         2
#define MGMT_ERR_EINVAL         3
#define MGMT_ERR_ETIMEOUT       4
#define MGMT_ERR_ENOENT         5
#define MGMT_ERR_EBADSTATE      6
#define MGMT_ERR_EMSGSIZE       7
#define MGMT_ERR_ENOTSUP        8
#define MGMT_ERR_ECORRUPT       9
#define MGMT_ERR_EBUSY          10
#define MGMT_ERR_EACCESSDENIED  11

struct smp_streamer;

typedef int (*mgmt_handler_fn)(struct smp_streamer *ctxt);

/** One command slot: a read handler, a write handler, or neither. */
struct mgmt_handler {
	mgmt_handler_fn mh_read;
	mgmt_handler_fn mh_write;
};

/** One management group and the commands it serves. */
struct mgmt_group {
	struct mgmt_handler *mg_handlers;
	uint16_t mg_handlers_count;
	uint16_t mg_group_id;
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	const char *mg_group_name;
#endif
};

void mgmt_register_group(struct mgmt_group *group);

#endif /* SMPFAKE_ZEPHYR_MGMT_MCUMGR_MGMT_H */
