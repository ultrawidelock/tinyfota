/* dfufake: <zephyr/sys/util.h>, the handful of macros tinyfota uses. Every one
 * is guarded: logfake's <zephyr/kernel.h> defines some of the same names and
 * either header may be included first. */
#ifndef DFUFAKE_ZEPHYR_SYS_UTIL_H
#define DFUFAKE_ZEPHYR_SYS_UTIL_H

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef ARG_UNUSED
#define ARG_UNUSED(x) (void)(x)
#endif
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
#ifndef ROUND_UP
#define ROUND_UP(x, align) ((((x) + ((align) - 1)) / (align)) * (align))
#endif
#ifndef ROUND_DOWN
#define ROUND_DOWN(x, align) (((x) / (align)) * (align))
#endif

/* On target this reaches dfu_applier.c through <zephyr/kernel.h> ->
 * zephyr/toolchain.h. logfake's kernel.h does not carry it, and the applier
 * includes this header, so it is defined here. Guarded: logfake's log.h
 * defines the same thing for the suites that include that instead. */
#ifndef BUILD_ASSERT
#define BUILD_ASSERT(cond, ...) _Static_assert((cond), "" __VA_ARGS__)
#endif

#ifndef IS_ENABLED
/* Zephyr's config probe: 1 iff the macro is defined to 1, else 0. */
#define DFUFAKE_IE3(ignore_this, val, ...) val
/* NOLINTNEXTLINE(bugprone-macro-parentheses) */
#define DFUFAKE_IE2(one_or_two_args)       DFUFAKE_IE3(one_or_two_args 1, 0)
#define _XXXX1                             DFUFAKE_YYYY,
#define DFUFAKE_IE1(config_macro)          DFUFAKE_IE2(_XXXX##config_macro)
#define IS_ENABLED(config_macro)           DFUFAKE_IE1(config_macro)
#endif

#endif /* DFUFAKE_ZEPHYR_SYS_UTIL_H */
