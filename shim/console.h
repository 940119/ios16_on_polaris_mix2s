#pragma once
#include <stdint.h>

/*
 * Early console framebuffer. sweet's splash region starts at 0x9c000000;
 * polaris (Mi MIX 2S) uses the 1080x2160 cont_splash_region at 0x9d400000.
 * Platform selects the geometry; the XNU early console takes the same values
 * from boot_args (patch 0083).
 */
#ifdef POLARIS
#define PLATFORM_NAME	"polaris"
#define FB_BASE		0x9d400000UL
#define FB_H		2160
#else
#define PLATFORM_NAME	"sweet"
#define FB_BASE		0x9c000000UL
#define FB_H		2400
#endif
#define FB_W		1080
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

/* Use the geometry the display hardware is actually scanning. */
void con_config(void *fb, int w, int h, int stride_bytes, int bpp);

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
void con_hex8(const char *label, uint32_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
