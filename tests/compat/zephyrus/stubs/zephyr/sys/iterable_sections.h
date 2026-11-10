/* Host stub for the zephyrus compatibility tests: no linker sections, so a
 * section is always empty (module names are not registered). */
#ifndef FORGE_COMPAT_ZEPHYR_ITERABLE_SECTIONS_H
#define FORGE_COMPAT_ZEPHYR_ITERABLE_SECTIONS_H
#define STRUCT_SECTION_ITERABLE(type, name) __attribute__((unused)) struct type name
#define STRUCT_SECTION_FOREACH(type, it) for (const struct type *it = 0; it != 0; it = 0)
#endif
