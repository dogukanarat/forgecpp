/* Host stub for the zephyrus compatibility tests. */
#ifndef FORGE_COMPAT_ZEPHYR_TOOLCHAIN_H
#define FORGE_COMPAT_ZEPHYR_TOOLCHAIN_H
#define BUILD_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
