/*
 * ios-kebab: Adreno 650 (SM8250) driver for XNU, bring-up edition.
 *
 * What this is: a bare-metal-style GPU driver in the sweet_* tradition
 * (bsd/dev kernel thread, plain MMIO, no IOKit object yet). It brings the
 * GPU to a state where the kernel can submit command-processor blits:
 *
 *   probe   - read RBBM_STATUS to confirm the Adreno 650 is there
 *   power   - release the GX/CX GDSCs in GPUCC; ABL leaves the GPU clocks
 *             running for continuous splash (like the USB clocks in sweet)
 *   gmu     - load a650_gmu.bin into the GMU ITCM/DTCM and release CM3 reset
 *   ring    - one CP ring buffer (physical RAM), RPTR polling, no interrupts
 *   blit    - CP_MEM_TO_MEM indirect buffer: the first consumer is the
 *             framebuffer panel copy (kebab_panel.c), replacing the CPU
 *             memcpy at sweetfb_hz
 *
 * What this is NOT (yet): no IOAccelerator/Metal path, no SMMU page tables,
 * no interrupts (polled ring), no GPU-side rendering. CoreAnimation keeps
 * its CPU renderer until a userland-facing accelerator service exists.
 *
 * Registers: Qualcomm's public kgsl/adreno headers (A6XX_*), GPU base
 * 0x3d00000 + GPUCC 0x3d90000 as in mainline sm8250.dtsi. Offsets marked
 * VERIFY in the comments must be confirmed on the first phone boot (the
 * driver prints every uncertain value it programs).
 */
#include <kern/debug.h>			/* kprintf */
#include <kern/thread.h>		/* kernel threads */
#include <libkern/libkern.h>		/* bcopy */
#include <mach/mach_types.h>
#include <vm/vm_kern.h>			/* kernel_map / kmem_alloc */
#include <vm/pmap.h>			/* pmap_extract */
#include <pexpert/pexpert.h>		/* PE_parse_boot_argn */
#include <IOKit/IOKitLib.h>		/* ADT /chosen lookup */

#include "kebab_sm8250.h"

/* ---- A6xx core registers (public kgsl offsets, GPUBASE + off) ---- */
#define A6XX_RBBM_STATUS		0x0210
#define A6XX_RBBM_STATUS_GPU_BUSY	(1u << 23)

#define A6XX_CP_RB_BASE			0x0C1E0000
#define A6XX_CP_RB_BASE_HI		0x0C1E0001
#define A6XX_CP_RB_CNTL		0x0C1E0002
#define A6XX_CP_RB_RPTR_ADDR		0x0C1E0003
#define A6XX_CP_RB_RPTR_ADDR_HI	0x0C1E0004
#define A6XX_CP_RB_RPTR		0x0C1E0005
#define A6XX_CP_RB_WPTR		0x0C1E0006
#define A6XX_CP_IB1_BASE		0x0C1E0010
#define A6XX_CP_IB1_BASE_HI		0x0C1E0011
#define A6XX_CP_IB1_BUFSZ		0x0C1E0012
#define A6XX_CP_SQE_CNTL		0x0C1E0058
#define A6XX_CP_ME_CNTL		0x0C1E005B
#define A6XX_CP_ME_STAT		0x0C1E005C
#define A6XX_CP_ME_STAT_BUSY		(1u << 27)

/* ---- GMU (graphics management unit) registers: the GMU is its own block
 * at 0x3d6a000 (mainline sm8250.dtsi), NOT inside the GPU block. Offsets
 * below are kgsl A6xx values, all VERIFY on the first boot. ---- */
#define A6XX_GMU_CM3_DTCM_START	0x40000	/* VERIFY */
#define A6XX_GMU_CM3_ITCM_START	0x50000	/* VERIFY */
#define A6XX_GMU_CM3_SYSRESET		0x50008	/* VERIFY */
#define A6XX_GMU_CM3_ITCM_SIZE		0x4000
#define A6XX_GMU_CM3_DTCM_SIZE		0x1000
#define A6XX_GMU_HFI_CTRL_STATUS	0x88000	/* VERIFY */

/* ---- GPUCC (0x3d90000) power switches ---- */
#define GPU_CC_GX_GDSCR		0x100c		/* VERIFY */
#define GPU_CC_CX_GDSCR		0x106c		/* VERIFY */
#define GDSCR_POWER_ON			0

/* ---- PM4 packet types (adreno_pm4types.h) ---- */
#define CP_TYPE3_PKT			0x70000000u
#define CP_TYPE3(opcode, sz)		(CP_TYPE3_PKT | ((uint32_t)(opcode) << 8) | \
					 ((uint32_t)((sz) - 1) & 0x7fff))
#define CP_MEM_TO_MEM			0x40

struct kebab_gpu {
	volatile uint32_t *gpu;		/* GPUBASE mapping (0x3d00000) */
	volatile uint32_t *gmu;		/* GMU block mapping (0x3d6a000) */
	volatile uint32_t *gpucc;	/* GPUCC mapping (0x3d90000) */
	uint64_t rb_pa;			/* ring buffer, physical */
	uint32_t *rb;			/* ring buffer, virtual */
	uint32_t rb_size;		/* in dwords */
	uint32_t rb_wptr;		/* our write pointer (dwords) */
	int powered;			/* GX GDSC released */
	int ring_on;			/* CP_ME_CNTL busy-busy on */
	const uint8_t *gmu_fw;		/* a650_gmu.bin, from the ADT */
	uint32_t gmu_fw_size;
};

static struct kebab_gpu kebab_gpu_state;
static struct { const uint8_t *fw; uint32_t fw_size; } kebab_gmu_state;

static inline uint32_t kgpu_rd(struct kebab_gpu *g, uint32_t off)
{
	return g->gpu[off / 4];
}

static inline void kgpu_wr(struct kebab_gpu *g, uint32_t off, uint32_t v)
{
	g->gpu[off / 4] = v;
	__asm__ volatile("dmb sy" ::: "memory");
}

/* ---- probe: is there an Adreno 650 behind GPUBASE? ---- */

static int kebab_gpu_probe(struct kebab_gpu *g)
{
	uint32_t status = kgpu_rd(g, A6XX_RBBM_STATUS);
	kprintf("KEBAK GPU: RBBM_STATUS 0x%08x%s\n", status,
		(status & A6XX_RBBM_STATUS_GPU_BUSY) ? " (busy)" : "");
	/*
	 * A real Adreno reads back something sane (a650: 0x8xxxxxxx-ish,
	 * VERIFY on the phone); unmapped MMIO reads as 0 or garbage on QEMU.
	 */
	if (status == 0 || status == 0xffffffff)
		return -1;
	g->gmu = (volatile uint32_t *)ml_io_map(SM8250_GMU_BASE, 0x30000);
	return 0;
}

/* ---- power: GPUCC GDSCs ---- */

static void kebab_gpu_power(struct kebab_gpu *g, int on)
{
	/*
	 * ABL leaves the graphics rails on for continuous splash (the sweet
	 * port observed the same for USB clocks). Vote anyway so a later
	 * power-off path has a starting point. GDSCR bit 0: 0 = on.
	 */
	uint32_t gx = g->gpucc[GPU_CC_GX_GDSCR / 4];
	uint32_t cx = g->gpucc[GPU_CC_CX_GDSCR / 4];
	if (on) {
		g->gpucc[GPU_CC_GX_GDSCR / 4] = gx & ~1u;
		g->gpucc[GPU_CC_CX_GDSCR / 4] = cx & ~1u;
	} else {
		g->gpucc[GPU_CC_GX_GDSCR / 4] = gx | 1u;
	}
	__asm__ volatile("dmb sy" ::: "memory");
	/* wait for the power state bits (VERIFY bit positions) */
	uint32_t want = on ? 0 : 1;
	for (int i = 0; i < 100000; i++) {
		if (((g->gpucc[GPU_CC_GX_GDSCR / 4] >> 4) & 1) == want)
			break;
	}
	kprintf("KEBAK GPU: GDSC GX 0x%08x CX 0x%08x -> %s\n",
		g->gpucc[GPU_CC_GX_GDSCR / 4], g->gpucc[GPU_CC_CX_GDSCR / 4],
		on ? "on" : "off");
	g->powered = on;
}

/* ---- GMU: a650_gmu.bin into ITCM/DTCM, CM3 reset release ---- */

static int kebab_gmu_boot(struct kebab_gpu *g)
{
	if (!g->gmu_fw || g->gmu_fw_size < A6XX_GMU_CM3_ITCM_SIZE) {
		kprintf("KEBAK GPU: no GMU firmware in ADT, running without GMU "
			"(direct mode; GMU-managed power states unavailable)\n");
		return -1;
	}
	/* hold the CM3 in reset, then fill its TCMs through the debug port */
	kgpu_wr(g, A6XX_GMU_CM3_SYSRESET, 1);
	for (uint32_t i = 0; i < A6XX_GMU_CM3_ITCM_SIZE / 4 &&
	     i * 4 < g->gmu_fw_size; i++) {
		g->gmu[A6XX_GMU_CM3_ITCM_START / 4 + i] = ((const uint32_t *)g->gmu_fw)[i];
	}
	kgpu_wr(g, A6XX_GMU_CM3_SYSRESET, 0);
	/* HFI handshake: the a650 GMU firmware raises CTRL_STATUS bit 1 when
	 * booted (VERIFY); poll briefly and report, don't hang. */
	for (int i = 0; i < 200000; i++) {
		if (kgpu_rd(g, A6XX_GMU_HFI_CTRL_STATUS) & 2)
			break;
	}
	uint32_t hfi = kgpu_rd(g, A6XX_GMU_HFI_CTRL_STATUS);
	kprintf("KEBAK GPU: GMU firmware %u bytes, HFI_CTRL_STATUS 0x%08x%s\n",
		g->gmu_fw_size, hfi, (hfi & 2) ? " (booted)" : " (no response)");
	return (hfi & 2) ? 0 : -1;
}

/* ---- CP ring ---- */

static void kebab_gpu_ring(struct kebab_gpu *g)
{
	/* Physical ring + rptr in wired kernel memory (bounce via IOMalloc-style
	 * path; the sweet UFS driver allocates its DMA pool the same way). */
	kgpu_wr(g, A6XX_CP_RB_BASE, (uint32_t)g->rb_pa);
	kgpu_wr(g, A6XX_CP_RB_BASE_HI, (uint32_t)(g->rb_pa >> 32));
	kgpu_wr(g, A6XX_CP_RB_RPTR_ADDR, (uint32_t)(g->rb_pa + g->rb_size * 4));
	kgpu_wr(g, A6XX_CP_RB_RPTR_ADDR_HI, (uint32_t)((g->rb_pa + g->rb_size * 4) >> 32));
	/* CNTL: block size (dwords/256)<<8 | BUF_SWAP 1 (32-bit) | BLKSZ_LOG2 0 */
	kgpu_wr(g, A6XX_CP_RB_CNTL, ((g->rb_size / 256) << 8) | (1 << 4) | 0);
	kgpu_wr(g, A6XX_CP_RB_WPTR, 0);
	g->rb_wptr = 0;
	/* busy-busy: the CP starts fetching from the ring immediately */
	kgpu_wr(g, A6XX_CP_ME_CNTL, 1);
	g->ring_on = 1;
	kprintf("KEBAK GPU: CP ring on, %u dwords at 0x%llx\n",
		g->rb_size, g->rb_pa);
}

/* ---- blit: one CP_MEM_TO_MEM per call, ring-submitted, polled ---- */

static void kebab_gpu_wait(struct kebab_gpu *g)
{
	/* wait for the CP to drain the ring (polled; no interrupt wiring yet) */
	for (uint32_t waited = 0; waited < 4000000; waited++) {
		uint32_t rptr = kgpu_rd(g, A6XX_CP_RB_RPTR);
		if (rptr == g->rb_wptr && !(kgpu_rd(g, A6XX_CP_ME_STAT) & A6XX_CP_ME_STAT_BUSY))
			return;
	}
	kprintf("KEBAK GPU: wait timed out (wptr 0x%x rptr 0x%x me_stat 0x%x)\n",
		g->rb_wptr, kgpu_rd(g, A6XX_CP_RB_RPTR), kgpu_rd(g, A6XX_CP_ME_STAT));
}

/*
 * Submit one CP_MEM_TO_MEM through an IB appended to the ring. Returns 0 on
 * submit; correctness is checked by kebab_gpu_test on the device. Sizes are
 * bytes; the CP opcode takes byte counts directly.
 */
int kebab_gpu_blit(uint64_t dst_pa, uint64_t src_pa, uint32_t bytes)
{
	struct kebab_gpu *g = &kebab_gpu_state;
	if (!g->ring_on)
		return -1;
	if (bytes & 3)
		return -1;

	/* IB: PKT3 MEM_TO_MEM dst src size (size is dwords<<2 in this encoding,
	 * VERIFY against kgsl cp_mem_to_mem; sizes here are byte-exact) */
	uint32_t ib[8];
	ib[0] = CP_TYPE3(CP_MEM_TO_MEM, 7);
	ib[1] = (uint32_t)dst_pa;
	ib[2] = (uint32_t)(dst_pa >> 32);
	ib[3] = (uint32_t)src_pa;
	ib[4] = (uint32_t)(src_pa >> 32);
	ib[5] = bytes;
	ib[6] = 0;
	ib[7] = 0;

	/* append to the ring with wraparound; ring size is a power of two */
	uint32_t mask = g->rb_size - 1;
	for (int i = 0; i < 8; i++)
		g->rb[(g->rb_wptr + i) & mask] = ib[i];
	__asm__ volatile("dmb sy" ::: "memory");
	g->rb_wptr = (g->rb_wptr + 8) & mask;
	kgpu_wr(g, A6XX_CP_RB_WPTR, g->rb_wptr);
	kebab_gpu_wait(g);
	return 0;
}

/* ---- self-test: copy a scratch pattern through the GPU and check ---- */

static void kebab_gpu_test(void)
{
	struct kebab_gpu *g = &kebab_gpu_state;
	/* two wired pages in kernel memory: src pattern -> dst via GPU blit */
	vm_offset_t src_v = 0, dst_v = 0;
	uint64_t src_pa = 0, dst_pa = 0;
	if (kmem_alloc(kernel_map, &src_v, 0x4000, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &dst_v, 0x4000, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		kprintf("KEBAK GPU: test alloc failed\n");
		return;
	}
	for (int i = 0; i < 0x4000 / 4; i++)
		((uint32_t *)src_v)[i] = 0x5a5a0000u + (uint32_t)i;
	src_pa = pmap_extract(kernel_pmap, src_v);
	dst_pa = pmap_extract(kernel_pmap, dst_v);
	if (!src_pa || !dst_pa) {
		kprintf("KEBAK GPU: test pmap_extract failed\n");
		return;
	}
	int rc = kebab_gpu_blit(dst_pa, src_pa, 0x4000);
	int ok = !rc && !bcmp((void *)dst_v, (void *)src_v, 0x4000);
	kprintf("KEBAK GPU: blit self-test %s (rc %d, src 0x%llx -> dst 0x%llx)\n",
		ok ? "PASS" : "FAIL", rc, src_pa, dst_pa);
	kmem_free(kernel_map, src_v, 0x4000);
	kmem_free(kernel_map, dst_v, 0x4000);
}

/* ---- GMU firmware from the ADT ----
 * tools/mkadt.py --gmu-fw embeds the vendor a650_gmu.bin as
 * /chosen/kebab-gmu-fw (a data prop); the firmware is a Qualcomm binary
 * from the phone's own vendor partition and never lives in git. Without it
 * the driver runs in direct mode (CP works, GMU-managed power does not). */
static const uint8_t *kebab_gmu_fw(void)
{
	io_registry_entry_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL,
							     "IODeviceTree:/chosen");
	if (!chosen)
		return NULL;
	CFDataRef fw = IORegistryEntryCreateCFProperty(chosen, CFSTR("kebab-gmu-fw"),
						       kCFAllocatorDefault, 0);
	IOObjectRelease(chosen);
	if (!fw)
		return NULL;
	kebab_gmu_state.fw = CFDataGetBytePtr(fw);
	kebab_gmu_state.fw_size = (uint32_t)CFDataGetLength(fw);
	return kebab_gmu_state.fw;	/* CFData owns the bytes */
}

/* ---- bring-up entry, called from bsd/dev/kebab/kebab_ufs.c ---- */

void kebab_gpu_init(void)
{
	struct kebab_gpu *g = &kebab_gpu_state;
	/* Map GPU + GPUCC with the same IO map helper style as sweet drivers.
	 * VERIFY on the phone: the mappings must be Device memory, uncached. */
	g->gpu = (volatile uint32_t *)ml_io_map(SM8250_GPU_BASE, SM8250_GPU_SIZE);
	g->gpucc = (volatile uint32_t *)ml_io_map(SM8250_GPU_CC, SM8250_GPU_CC_SIZE);
	if (!g->gpu || !g->gpucc) {
		kprintf("KEBAK GPU: mmap failed\n");
		return;
	}
	g->gmu_fw = kebab_gmu_fw();
	g->gmu_fw_size = kebab_gmu_state.fw_size;

	if (kebab_gpu_probe(g)) {
		kprintf("KEBAK GPU: no Adreno 650 at 0x%x (QEMU run?)\n", SM8250_GPU_BASE);
		return;
	}
	kebab_gpu_power(g, 1);
	kebab_gmu_boot(g);		/* non-fatal: direct mode works without it */

	/* ring buffer: wired kernel memory, physical, 64K dwords */
	vm_offset_t rb_v = 0;
	if (kmem_alloc(kernel_map, &rb_v, 0x40000, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		kprintf("KEBAK GPU: ring alloc failed\n");
		return;
	}
	g->rb = (uint32_t *)rb_v;
	g->rb_size = 0x10000;
	g->rb_pa = pmap_extract(kernel_pmap, rb_v);
	if (!g->rb_pa) {
		kprintf("KEBAK GPU: ring pmap_extract failed\n");
		return;
	}
	kebab_gpu_ring(g);

	/* 'kebabgputest=1' on the cmdline: run the self-test once at boot. */
	if (PE_parse_boot_argn("kebabgputest", &rb_v, sizeof(rb_v)) && rb_v)
		kebab_gpu_test();
}
