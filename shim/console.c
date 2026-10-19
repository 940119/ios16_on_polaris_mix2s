/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 *
 * Geometry is runtime-configurable: ABL may leave the MDP scanning a
 * buffer whose stride/bpp differ from simplefb's 1080x2160 ARGB8888.
 */
#include "console.h"
#define FONT_H_DEFINE
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
static volatile uint8_t *con_base = (volatile uint8_t *)FB_BASE;
static int con_w = FB_W, con_h = FB_H;
static int con_stride = FB_STRIDE, con_bpp = 4;
static uint32_t con_bg;
static int row;

void con_config(void *fb, int w, int h, int stride_bytes, int bpp)
{
	con_base = (volatile uint8_t *)fb;
	con_fb = (volatile uint32_t *)fb;
	con_w = w;
	con_h = h;
	con_stride = stride_bytes;
	con_bpp = bpp;
}

static uint16_t to565(uint32_t c)
{
	return (uint16_t)(((c >> 8) & 0xf800) |
			  ((c >> 5) & 0x07e0) |
			  ((c >> 3) & 0x001f));
}

static void put_px(int x, int y, uint32_t c)
{
	if (x < 0 || y < 0 || x >= con_w || y >= con_h)
		return;
	volatile uint8_t *p = con_base + (uintptr_t)y * con_stride +
			      (uintptr_t)x * con_bpp;
	if (con_bpp == 4) {
		*(volatile uint32_t *)p = c;
	} else if (con_bpp == 2) {
		*(volatile uint16_t *)p = to565(c);
	} else if (con_bpp == 3) {
		p[0] = (uint8_t)(c >> 16);
		p[1] = (uint8_t)(c >> 8);
		p[2] = (uint8_t)c;
	}
}

void con_clear(uint32_t bg)
{
	for (int y = 0; y < con_h; y++)
		for (int x = 0; x < con_w; x++)
			put_px(x, y, bg);
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > con_w || y0 + CH_H > con_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					put_px(x0 + gx * SCALE + sx,
					       y0 + gy * SCALE + sy, c);
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex8(const char *label, uint32_t v)
{
	char buf[9];
	fmt_hex(buf, v, 8);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}
