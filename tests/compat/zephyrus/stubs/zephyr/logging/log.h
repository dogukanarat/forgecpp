/* Host stub for the zephyrus compatibility tests: logging writes nothing. */
#ifndef FORGE_COMPAT_ZEPHYR_LOG_H
#define FORGE_COMPAT_ZEPHYR_LOG_H
#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#endif
