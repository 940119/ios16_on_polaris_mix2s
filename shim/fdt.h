#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* RAM ranges of the root /memory node (ABL patches the zeros in the stock DTB). */
int fdt_memory_ranges(uintptr_t fdt, uint64_t *base, uint64_t *size, int max);

/* Root /model string; returns 0 when absent. */
int fdt_model(uintptr_t fdt, char *out, unsigned int cap);
