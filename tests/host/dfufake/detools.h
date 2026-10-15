/* dfufake: <detools.h> as a SCRIPTED DOUBLE, not the vendored patcher.
 *
 * dfu_applier.c's entire contract with detools is five callbacks, and every
 * interesting line in that file is reached through them: the write combiner
 * (arbitrary address, arbitrary length), the flush a read forces, the page
 * rounding on erase, the append-only step log, and the resume gate. Replaying
 * a scripted list of operations exercises all of it deterministically.
 *
 * Running the REAL detools instead would need a real in-place patch, which
 * needs the python `detools` package to generate and a committed binary
 * fixture to carry — and would still exercise only whichever operation
 * sequence that one patch happens to contain, with no way to reach the
 * failure paths at all. So: the double drives the callbacks, and the suite
 * asserts on what reached the flash. Nothing here validates detools' own
 * patch format, and nothing here claims to. */
#ifndef DFUFAKE_DETOOLS_H
#define DFUFAKE_DETOOLS_H

#include <stddef.h>
#include <stdint.h>

typedef int (*detools_mem_read_t)(void *arg_p, void *dst_p, uintptr_t src, size_t size);
typedef int (*detools_mem_write_t)(void *arg_p, uintptr_t dst, void *src_p, size_t size);
typedef int (*detools_mem_erase_t)(void *arg_p, uintptr_t addr, size_t size);
typedef int (*detools_step_set_t)(void *arg_p, int step);
typedef int (*detools_step_get_t)(void *arg_p, int *step_p);

/** The patcher object. Only the callback table and arg are used by the double. */
struct detools_apply_patch_in_place_t {
	detools_mem_read_t mem_read;
	detools_mem_write_t mem_write;
	detools_mem_erase_t mem_erase;
	detools_step_set_t step_set;
	detools_step_get_t step_get;
	void *arg_p;
	size_t patch_size;
	int replayed;
};

int detools_apply_patch_in_place_init(struct detools_apply_patch_in_place_t *self_p,
				      detools_mem_read_t mem_read, detools_mem_write_t mem_write,
				      detools_mem_erase_t mem_erase, detools_step_set_t step_set,
				      detools_step_get_t step_get, size_t patch_size, void *arg_p);

int detools_apply_patch_in_place_process(struct detools_apply_patch_in_place_t *self_p,
					 const uint8_t *patch_p, size_t size);

int detools_apply_patch_in_place_finalize(struct detools_apply_patch_in_place_t *self_p);

#endif /* DFUFAKE_DETOOLS_H */
