/* Replaces the private header of zephyrus's c2 module (src/c2.h) for the
 * compatibility tests: only the codec in c2.c is built, without the
 * dispatcher and the services that need a Zephyr kernel. */
#ifndef ZEPHYRUS_C2_PRIVATE_H
#define ZEPHYRUS_C2_PRIVATE_H
#include <zephyrus/c2/c2.h>
#include <zephyrus/common_module_id.h>
#define MODULE_ID CONFIG_ZEPHYRUS_C2_MODULE_ID
#endif
