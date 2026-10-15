/* dfufake — the scripted detools double and the running-image buffer. The
 * flash and reboot halves this file used to carry are the flash host
 * backend now (tests/host/port/flash_host.c); see dfufake.h.
 */

#include "dfufake.h"

#include <string.h>

#include <detools.h>

#include "tinyfota_host.h"

struct dfufake_state dfufake;

/* dfu_smp_img.c reads the running image through PM_MCUBOOT_PAD_ADDRESS. One
 * page is more than enough for a header plus a TLV block. */
uint8_t dfufake_running_image[DFUFAKE_PAGE_SIZE];

void dfufake_blank(struct tinyfota_flash_host_area *area)
{
	memset(area->buf, 0xff, area->size);
}

uint8_t dfufake_peek(const struct tinyfota_flash_host_area *area, size_t off)
{
	return off < area->size ? area->buf[off] : 0;
}

void dfufake_reset(void)
{
	memset(&dfufake, 0, sizeof(dfufake));
	tinyfota_flash_host_reset();
	tinyfota_osal_host_reset();
	memset(dfufake_running_image, 0, sizeof(dfufake_running_image));
	dfufake_script(NULL, 0);
}

/* ---- scripted detools ----------------------------------------------------- */

static struct dfufake_op script_ops[DFUFAKE_MAX_OPS];
static size_t script_count;
static size_t script_done;
static int script_last_step_get;

void dfufake_script(const struct dfufake_op *ops, size_t count)
{
	if (count > DFUFAKE_MAX_OPS) {
		count = DFUFAKE_MAX_OPS;
	}
	if (ops != NULL && count > 0U) {
		memcpy(script_ops, ops, count * sizeof(*ops));
	}
	script_count = (ops != NULL) ? count : 0U;
	script_done = 0;
	script_last_step_get = -1;
}

int dfufake_last_step_get(void)
{
	return script_last_step_get;
}

size_t dfufake_ops_done(void)
{
	return script_done;
}

int detools_apply_patch_in_place_init(struct detools_apply_patch_in_place_t *self_p,
				      detools_mem_read_t mem_read, detools_mem_write_t mem_write,
				      detools_mem_erase_t mem_erase, detools_step_set_t step_set,
				      detools_step_get_t step_get, size_t patch_size, void *arg_p)
{
	dfufake.detools_init_calls++;
	dfufake.detools_patch_size = patch_size;
	dfufake.detools_fed = 0;

	self_p->mem_read = mem_read;
	self_p->mem_write = mem_write;
	self_p->mem_erase = mem_erase;
	self_p->step_set = step_set;
	self_p->step_get = step_get;
	self_p->arg_p = arg_p;
	self_p->patch_size = patch_size;
	self_p->replayed = 0;

	return dfufake.detools_init_ret;
}

/* Scratch for DFUFAKE_OP_WRITE / DFUFAKE_OP_READ payloads. One page covers any
 * operation a suite has reason to script. */
static uint8_t op_scratch[DFUFAKE_PAGE_SIZE];

static int replay(struct detools_apply_patch_in_place_t *self_p)
{
	for (; script_done < script_count; script_done++) {
		const struct dfufake_op *op = &script_ops[script_done];
		size_t len = op->len;
		int rc;

		if (len > sizeof(op_scratch)) {
			len = sizeof(op_scratch);
		}

		switch (op->kind) {
		case DFUFAKE_OP_WRITE:
			memset(op_scratch, op->fill, len);
			rc = self_p->mem_write(self_p->arg_p, op->addr, op_scratch, len);
			break;
		case DFUFAKE_OP_READ:
			rc = self_p->mem_read(self_p->arg_p, op_scratch, op->addr, len);
			break;
		case DFUFAKE_OP_ERASE:
			rc = self_p->mem_erase(self_p->arg_p, op->addr, op->len);
			break;
		case DFUFAKE_OP_STEP_SET:
			rc = self_p->step_set(self_p->arg_p, op->step);
			break;
		case DFUFAKE_OP_STEP_GET:
			rc = self_p->step_get(self_p->arg_p, &script_last_step_get);
			break;
		default:
			rc = -1;
			break;
		}

		if (rc != 0) {
			script_done++;
			return DFUFAKE_DETOOLS_CALLBACK_FAILED;
		}
	}
	return 0;
}

int detools_apply_patch_in_place_process(struct detools_apply_patch_in_place_t *self_p,
					 const uint8_t *patch_p, size_t size)
{
	(void)patch_p;
	dfufake.detools_process_calls++;
	dfufake.detools_fed += size;

	if (dfufake.detools_process_ret != 0) {
		return dfufake.detools_process_ret;
	}
	/* The script runs once, on the first chunk: the applier streams the
	 * patch in CONFIG_TINYFOTA_APPLIER_CHUNK pieces and replaying per chunk
	 * would multiply every scripted operation by the chunk count. */
	if (self_p->replayed) {
		return 0;
	}
	self_p->replayed = 1;
	return replay(self_p);
}

int detools_apply_patch_in_place_finalize(struct detools_apply_patch_in_place_t *self_p)
{
	(void)self_p;
	dfufake.detools_finalize_calls++;
	return dfufake.detools_finalize_ret;
}
