/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location) and how much RAM this board has.
 * Big-endian, 4-byte aligned tokens; loads are byte-wise because the tree
 * arrives as Device memory and may not be aligned to the access size.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	const volatile uint8_t *p = (const volatile uint8_t *)a;
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(uintptr_t a)
{
	return (uint64_t)be32(a) << 32 | be32(a + 4);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int starts_with(const char *s, const char *prefix)
{
	while (*prefix)
		if (*s++ != *prefix++)
			return 0;
	return 1;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = be64(val);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

/*
 * RAM ranges from the root memory node, which ABL fills in before booting.
 * Returns the number of ranges written (up to max). The 6/8GB question for
 * mkadt --ram is answered by what this prints on the probe screen.
 */
int fdt_memory_ranges(uintptr_t fdt, uint64_t *base, uint64_t *size, int max)
{
	if (!fdt || be32(fdt) != FDT_MAGIC || max <= 0)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, in_memory = 0, n = 0;
	uint32_t ac = 2, sc = 2;	/* arm64 defaults */

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			if (depth == 1) {
				ac = sc = 2;
			} else if (depth == 2) {
				in_memory = streq(name, "memory") || starts_with(name, "memory@");
			}
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (depth == 2)
				in_memory = 0;
			depth--;
			if (depth < 0)
				return n;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			const char *name = (const char *)(strings + nameoff);
			if (depth == 1 && streq(name, "#address-cells") && len == 4)
				ac = be32(val);
			else if (depth == 1 && streq(name, "#size-cells") && len == 4)
				sc = be32(val);
			else if (in_memory && depth == 2 && streq(name, "reg")) {
				uint32_t cells = ac + sc;
				if (cells == 0 || cells > 8)
					return n;
				for (uint32_t off = 0; off + cells * 4 <= len && n < max; off += cells * 4) {
					uint64_t b = 0, s = 0;
					for (uint32_t i = 0; i < ac; i++)
						b = b << 32 | be32(val + off + i * 4);
					for (uint32_t i = 0; i < sc; i++)
						s = s << 32 | be32(val + off + (ac + i) * 4);
					if (s) {
						base[n] = b;
						size[n] = s;
						n++;
					}
				}
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return n;
		}
	}
}

int fdt_model(uintptr_t fdt, char *out, unsigned int cap)
{
	if (!fdt || be32(fdt) != FDT_MAGIC || cap == 0)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0;

	out[0] = 0;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			depth++;
			p = align4(skip_str(p));
			break;
		case FDT_END_NODE:
			depth--;
			if (depth <= 0)
				return out[0] != 0;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (depth == 1 && streq((const char *)(strings + nameoff), "model")) {
				unsigned int i;
				for (i = 0; i + 1 < len && i + 1 < cap; i++) {
					char c = *(volatile char *)(val + i);
					out[i] = c;
					if (!c)
						return 1;
				}
				out[i] = 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return out[0] != 0;
		}
	}
}
