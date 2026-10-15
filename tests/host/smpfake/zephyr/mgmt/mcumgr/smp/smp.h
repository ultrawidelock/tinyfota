/* smpfake: <zephyr/mgmt/mcumgr/smp/smp.h> — the streamer a handler is handed.
 * Only the two zcbor states are reachable from it, which is all
 * dfu_smp_img.c ever touches. */
#ifndef SMPFAKE_ZEPHYR_MGMT_MCUMGR_SMP_H
#define SMPFAKE_ZEPHYR_MGMT_MCUMGR_SMP_H

#include <zcbor_common.h>

/** Stands in for cbor_nb_reader / cbor_nb_writer; `zs` is an array on target. */
struct smpfake_nb {
	zcbor_state_t zs[2];
};

/** What mcumgr hands a command handler. */
struct smp_streamer {
	struct smpfake_nb *reader;
	struct smpfake_nb *writer;
};

#endif /* SMPFAKE_ZEPHYR_MGMT_MCUMGR_SMP_H */
