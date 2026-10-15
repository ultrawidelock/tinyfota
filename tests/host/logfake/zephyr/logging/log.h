/* logfake: minimal <zephyr/logging/log.h>. */
#ifndef LOGFAKE_ZEPHYR_LOGGING_LOG_H
#define LOGFAKE_ZEPHYR_LOGGING_LOG_H

#include <stdint.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define LOG_LEVEL_NONE 0u
#define LOG_LEVEL_ERR  1u
#define LOG_LEVEL_WRN  2u
#define LOG_LEVEL_INF  3u
#define LOG_LEVEL_DBG  4u

/* No-op module/log macros. Guarded: tinyfota_port.h defines them too. */
#ifndef LOG_MODULE_REGISTER
#define LOG_MODULE_REGISTER(...)
#endif
#ifndef LOG_MODULE_DECLARE
#define LOG_MODULE_DECLARE(...)
#endif
#ifndef LOG_ERR
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#endif

/*
 * On target this arrives through zephyr/toolchain.h, which the real log.h pulls
 * in -- so a source that uses BUILD_ASSERT and includes only log.h compiles
 * there and would not compile here. Guarded, because a suite that already has a
 * real definition must keep it.
 */
#ifndef BUILD_ASSERT
#define BUILD_ASSERT(cond, ...) _Static_assert((cond), "" __VA_ARGS__)
#endif

#endif /* LOGFAKE_ZEPHYR_LOGGING_LOG_H */
