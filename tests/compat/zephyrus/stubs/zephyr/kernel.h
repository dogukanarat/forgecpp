/* Host stub for the zephyrus compatibility tests: the tests call from one
 * thread, so a mutex does nothing. */
#ifndef FORGE_COMPAT_ZEPHYR_KERNEL_H
#define FORGE_COMPAT_ZEPHYR_KERNEL_H
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
struct k_mutex {
    int locked;
};
typedef int k_timeout_t;
#define K_FOREVER (-1)
#define K_MUTEX_DEFINE(name) static struct k_mutex name
static inline int k_mutex_init(struct k_mutex *m) { m->locked = 0; return 0; }
static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t t) { (void)t; m->locked = 1; return 0; }
static inline int k_mutex_unlock(struct k_mutex *m) { m->locked = 0; return 0; }
#endif
