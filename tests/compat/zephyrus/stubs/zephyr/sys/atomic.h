/* Host stub for the zephyrus compatibility tests (one thread). */
#ifndef FORGE_COMPAT_ZEPHYR_ATOMIC_H
#define FORGE_COMPAT_ZEPHYR_ATOMIC_H
typedef long atomic_t;
#define ATOMIC_INIT(value) (value)
static inline long atomic_inc(atomic_t *target) { return (*target)++; }
#endif
