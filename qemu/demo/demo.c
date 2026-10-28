/*
 * ios-kebab demo kernel: a stand-in for XNU in the shim's SWEETXNU payload
 * path. It runs wherever the shim puts it (physBase + (va - virtBase)) with
 * the MMU off, so it uses no .bss, no globals and only PC-relative data
 * references. It shows the whole boot chain works end to end:
 *
 *   QEMU/ABL -> shim (Image) -> SWEETXNU payload (Mach-O + ADT + cmdline)
 *            -> this demo: serial + framebuffer + ADT walk + heartbeat
 *
 * When a real XNU kernel exists, tools/mkpayload.py packs it the same way
 * and the shim boots it unchanged - this file just proves the plumbing.
 */
#include <stdint.h>
#include "font.h"	/* shim's font8x16[95][16], chars 32..126 */

#define UART_BASE	0x09000000UL	/* QEMU virt PL011 */
#define UART_DR		(*(volatile uint32_t *)(UART_BASE + 0x00))
#define UART_FR		(*(volatile uint32_t *)(UART_BASE + 0x18))
#define FB		0x9c000000UL	/* boot_args.Video.v_baseAddr (cont_splash) */
#define FB_W		1080
#define FB_H		2400
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8 */

#define BLACK	0xff000000U
#define WHITE	0xffffffffU
#define GREEN	0xff00ff00U
#define YELLOW	0xffffff00U
#define BLUE	0xff0000a0U

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 - what the shim fills in */
struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct {
		uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
	} Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[608];
	uint64_t bootFlags, memSizeActual;
};

/* ---- PL011 serial ---- */

static void uart_putc(char c)
{
	if (c == '\n')
		uart_putc('\r');
	while (!(UART_FR & (1u << 7)))	/* TXFE: transmit FIFO empty */
		;
	UART_DR = c;
}

static void uart_puts(const char *s)
{
	while (*s)
		uart_putc(*s++);
}

static void uart_hex(uint64_t v, int ndigits)
{
	static const char hex[] = "0123456789abcdef";
	char buf[16];
	int i = 0;
	if (ndigits > 15)
		ndigits = 15;
	do {
		buf[i++] = hex[v & 0xf];
		v >>= 4;
	} while (v);
	while (i < ndigits)
		buf[i++] = '0';
	while (i)
		uart_putc(buf[--i]);
}

/* ---- framebuffer ---- */

static void putpix(int x, int y, uint32_t c)
{
	*(volatile uint32_t *)(FB + (uint64_t)y * FB_STRIDE + (uint64_t)x * 4) = c;
}

static void fill(int x, int y, int w, int h, uint32_t c)
{
	for (int yy = 0; yy < h; yy++)
		for (int xx = 0; xx < w; xx++)
			putpix(x + xx, y + yy, c);
}

/* one glyph, 2x2 scaled: cell 16x32 px, 67x75 cells on the 1080x2400 panel */
static void draw_char(int col, int row, char c, uint32_t fg)
{
	int g = c - 32;
	if (c < 32 || c > 126)
		return;
	int x0 = col * 16, y0 = row * 32;
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 8; x++)
			if (font8x16[g][y] & (1u << (7 - x)))
				for (int dy = 0; dy < 2; dy++)
					for (int dx = 0; dx < 2; dx++)
						putpix(x0 + x * 2 + dx, y0 + y * 2 + dy, fg);
}

static int col, row;

static void draw_str(const char *s, uint32_t fg)
{
	while (*s && row < FB_H / 32) {
		if (*s == '\n') {
			col = 0;
			row++;
		} else if (col >= FB_W / 16) {
			col = 0;
			row++;
			draw_char(col++, row, *s, fg);
		} else {
			draw_char(col++, row, *s, fg);
		}
		s++;
	}
}

static void draw_hex(const char *label, uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	char buf[24];
	int i = 0;
	while (label[i])
		buf[i] = label[i], i++;
	buf[i++] = '0', buf[i++] = 'x';
	for (int d = 15; d >= 0; d--)
		buf[i++] = hex[(v >> (d * 4)) & 0xf];
	buf[i] = 0;
	draw_str(buf, WHITE);
	draw_str("\n", 0);
}

/* ---- Apple Device Tree walk (tools/adt.py layout) ---- */

struct adt_prop {
	char name[32];
	uint32_t size;		/* bit31 = placeholder flag */
	/* value[size], padded to 4 */
};

struct adt_node {
	uint32_t nprops, nchildren;
	/* props..., children... */
};

/* byte offset just past the node at p: its props and every child subtree */
static uintptr_t node_end(uintptr_t p)
{
	volatile struct adt_node *n = (volatile struct adt_node *)p;
	uintptr_t q = p + 8;
	for (uint32_t i = 0; i < n->nprops; i++) {
		volatile struct adt_prop *pr = (volatile struct adt_prop *)q;
		q += 32 + 4 + ((pr->size + 3) & ~3U);
	}
	for (uint32_t i = 0; i < n->nchildren; i++)
		q = node_end(q);
	return q;
}

static void walk_adt(uintptr_t p, int depth, int *count)
{
	if (depth > 5 || *count >= 16)
		return;
	uintptr_t q = p + 8;
	volatile struct adt_node *n = (volatile struct adt_node *)p;
	for (uint32_t i = 0; i < n->nprops; i++) {
		volatile struct adt_prop *pr = (volatile struct adt_prop *)q;
		q += 32 + 4 + ((pr->size + 3) & ~3U);
	}
	for (uint32_t i = 0; i < n->nchildren; i++) {
		volatile struct adt_prop *pr = (volatile struct adt_prop *)(q + 8);
		const char *name = (const char *)((uintptr_t)pr + 36);	/* the "name" prop's value */
		char line[40];
		int l = 0;
		for (int d = 0; d < depth; d++)
			line[l++] = ' ';
		line[l++] = '/';
		for (int k = 0; k < 24 && l < 36 && name[k]; k++)
			line[l++] = name[k];
		line[l] = 0;
		uart_puts(line);
		uart_puts("\n");
		(*count)++;
		walk_adt(q, depth + 1, count);
		q = node_end(q);	/* skip the whole child subtree to its sibling */
	}
}

/* ---- timing ---- */

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

/* ---- main ---- */

void demo_main(struct boot_args *ba)
{
	static const char banner[] = "ios-kebab demo kernel (shim -> SWEETXNU payload)";
	uint64_t delta = ba->physBase - ba->virtBase;	/* pa = va + delta */

	uart_puts("\n\n");
	uart_puts(banner);
	uart_puts("\n");

	fill(0, 0, FB_W, FB_H, BLACK);
	col = 0;
	row = 0;
	draw_str(banner, GREEN);
	draw_str("\n", 0);
	draw_hex("physBase ", ba->physBase);
	draw_hex("virtBase ", ba->virtBase);
	draw_hex("memSize  ", ba->memSize);
	draw_hex("video    ", ba->Video.v_baseAddr);
	draw_hex("deviceTre", ba->deviceTreeP);
	draw_str("cmdline: ", YELLOW);
	draw_str(ba->CommandLine, BLUE);
	draw_str("\n", 0);

	uart_puts("boot_args: physBase=");
	uart_hex(ba->physBase, 16);
	uart_puts(" memSize=");
	uart_hex(ba->memSize, 16);
	uart_puts(" video=");
	uart_hex(ba->Video.v_baseAddr, 16);
	uart_puts("\ncmdline: ");
	uart_puts(ba->CommandLine);
	uart_puts("\n");

	/* Walk the generated Apple Device Tree (physical = deviceTreeP + delta). */
	uintptr_t adt = ba->deviceTreeP + delta;
	uart_puts("ADT nodes:\n");
	{
		volatile struct adt_prop *pr = (volatile struct adt_prop *)(adt + 8);
		uart_puts("/");
		uart_puts((const char *)((uintptr_t)pr + 36));	/* the "name" prop's value */
		uart_puts("\n");
	}
	int count = 1;
	walk_adt(adt, 1, &count);
	uart_puts("ADT walk done\n");

	draw_str("\nADT: ", YELLOW);
	{
		char nbuf[4];
		nbuf[0] = '0' + (count / 10) % 10;
		nbuf[1] = '0' + count % 10;
		nbuf[2] = 0;
		draw_str(nbuf, WHITE);
	}
	draw_str(" nodes listed on serial", WHITE);
	draw_str("\n\nheartbeat ticks:\n", YELLOW);

	/* Heartbeat: a progress bar + hex tick counter + periodic serial line. */
	uint64_t t = 0;
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	for (;;) {
		uint64_t end = ticks() + f / 4;		/* 250 ms per tick */
		fill(0, 70 * 32, FB_W, 6, BLACK);	/* progress bar row */
		for (int x = 0; x < (int)(t % (FB_W / 4)); x++)
			fill(x * 4, 70 * 32, 4, 6, t < 40 ? GREEN : YELLOW);

		col = 0;
		row = 72;
		draw_hex("tick ", t);
		row++;
		fill(0, row * 32, 20 * 16, 32, BLACK);
		col = 0;
		draw_hex("freq ", f);

		uart_puts("tick ");
		uart_hex(t, 8);
		uart_puts("\n");
		t++;
		while (ticks() < end)
			;
	}
}
