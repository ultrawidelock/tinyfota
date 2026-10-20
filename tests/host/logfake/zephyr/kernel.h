/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/* logfake: minimal <zephyr/kernel.h> for host-building the SMP image group
 * (ports/zephyr/dfu_smp_img.c). Trimmed to what that file reaches. */
#ifndef LOGFAKE_ZEPHYR_KERNEL_H
#define LOGFAKE_ZEPHYR_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef ARG_UNUSED
#define ARG_UNUSED(x) (void)(x)
#endif

/* snprintk is snprintf with a trimmed format set; host snprintf covers it. */
#define snprintk snprintf

#include <errno.h>

#ifndef printk
#define printk printf
#endif

#ifndef IS_ENABLED
/* Zephyr's config-macro probe: 1 iff the macro is defined to 1, else 0. */
#define LOGFAKE_IE3(ignore_this, val, ...) val
/* The token-paste probe cannot parenthesize its argument. */
/* NOLINTNEXTLINE(bugprone-macro-parentheses) */
#define LOGFAKE_IE2(one_or_two_args)       LOGFAKE_IE3(one_or_two_args 1, 0)
#define _XXXX1                             LOGFAKE_YYYY,
#define LOGFAKE_IE1(config_macro)          LOGFAKE_IE2(_XXXX##config_macro)
#define IS_ENABLED(config_macro)           LOGFAKE_IE1(config_macro)
#endif

/* No k_work surface here, deliberately. Deferred work in the core goes
 * through tinyfota_port.h, whose host backend (tests/host/port/osal_host.c) IS
 * the fake. */

#endif /* LOGFAKE_ZEPHYR_KERNEL_H */
