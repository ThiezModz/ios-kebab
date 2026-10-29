/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/*
 * Android sparse image support (AOSP system/core/libsparse). The boot.img
 * v2 header stores the ramdisk size in a u32, so payloads above 4 GiB ship
 * as a sparse blob (tools: img2simg) and ABL may hand either form to us.
 * The unsparsed payload is LONGER than the blob, so the backward pass below
 * is safe in place: at every step the write cursor stays ahead of the read
 * cursor. Returns the unsparsed size, or 0 if this is not a sparse blob.
 */
#define SPARSE_MAGIC 0xED26FF3A
#define SPARSE_RAW 0xCAC1
#define SPARSE_FILL 0xCAC2
#define SPARSE_DONT_CARE 0xCAC3
#define SPARSE_CRC32 0xCAC4

struct sparse_header {
	uint32_t magic;
	uint16_t major, minor;
	uint16_t file_hdr_sz, chunk_hdr_sz;
	uint32_t blk_sz, total_blks, total_chunks, checksum;
} __attribute__((packed));

struct sparse_chunk {
	uint16_t type, reserved;
	uint32_t chunk_sz, total_sz;
} __attribute__((packed));

static uint64_t unsparse(uint64_t base)
{
	const struct sparse_header *h = (const struct sparse_header *)base;

	if (h->magic != SPARSE_MAGIC || h->major != 1 || h->file_hdr_sz < sizeof(*h) ||
	    h->chunk_hdr_sz < sizeof(struct sparse_chunk) || h->blk_sz == 0 || h->total_blks == 0)
		return 0;

	const struct sparse_chunk *chunks = (const struct sparse_chunk *)(base + h->file_hdr_sz);
	const uint8_t *in = (const uint8_t *)(chunks + h->total_chunks);
	uint64_t in_bytes_total = 0;

	for (uint32_t i = 0; i < h->total_chunks; i++)
		in_bytes_total += chunks[i].total_sz;
	in += in_bytes_total;	/* the cursor starts at the END of the input */

	const uint64_t out_size = (uint64_t)h->total_blks * h->blk_sz;
	uint64_t out_pos = out_size;
	con_hex2("SPARSE", "blks=", h->total_blks, "chunks=", h->total_chunks);

	/* Walk the chunks backward: chunk i covers [start, end) of the output. */
	for (uint32_t i = h->total_chunks; i-- > 0;) {
		const struct sparse_chunk *c = &chunks[i];
		uint64_t seg_bytes = (uint64_t)c->chunk_sz * h->blk_sz;
		uint64_t in_bytes = c->total_sz;
		uint64_t start = out_pos - seg_bytes;
		const uint8_t *src = in - in_bytes;

		if (start > out_pos)		/* arithmetic or format corruption */
			return 0;
		if (c->type == SPARSE_RAW) {
			copy64(base + start, (uintptr_t)src, in_bytes);
		} else if (c->type == SPARSE_FILL) {
			if (in_bytes != 4)
				return 0;
			volatile uint32_t *d = (volatile uint32_t *)(base + start);
			for (uint64_t o = 0; o < seg_bytes; o += 4)
				d[o / 4] = *(const uint32_t *)src;
		} else if (c->type == SPARSE_DONT_CARE) {
			zero64(base + start, seg_bytes);
		} else if (c->type == SPARSE_CRC32) {
			if (in_bytes < 4)
				return 0;
			copy64(base + start, (uintptr_t)src, in_bytes - 4);
		} else {
			return 0;
		}
		out_pos = start;
		in = src;
	}
	return out_pos == 0 ? out_size : 0;
}

const char *loader_boot_at(uintptr_t dtb, const struct payload *p, uint64_t rd_start);

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic)) {
		/* The boot.img may carry the payload as an Android sparse image. */
		if (unsparse(rd_start) == 0)
			return "no SWEETXNU payload";
		p = (const struct payload *)rd_start;
		if (!magic_ok(p->magic))
			return "no SWEETXNU payload after unsparse";
	}
	return loader_boot_at(dtb, p, rd_start);
}

/*
 * The payload placement itself: validate, copy the segments to
 * phys_base + (vmaddr - virt_base), place the ADT and boot_args behind the
 * kernel, jump with the MMU and caches off and x0 = boot_args (physical).
 * rd_start is where the payload file lives (ABL ramdisk or the USB slot).
 */
const char *loader_boot_at(uintptr_t dtb, const struct payload *p, uint64_t rd_start)
{
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload file spans rd_start .. rd_start+adt_off+adt_size (the ADT
	 * is the last thing in it). It must not overlap where the kernel goes. */
	uint64_t rd_end = rd_start + p->adt_off + p->adt_size;
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
