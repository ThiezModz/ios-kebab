/*
 * ios-kebab: OnePlus 8T display driver (DPU + DSI0 + Samsung AMB655X panel).
 *
 * Panel (kebab dts: dsi-panel-oplus20828-samsung-amb655x): Samsung AMB655X,
 * 1080x2400, 60/120 Hz, DSI COMMAND mode, 4 lanes, DSC 8bpc/8bpp 2 slices
 * (540x30). Command mode is the big difference from sweet: a video-mode
 * panel scans the splash memory out by itself, so sweet only had to memcpy
 * frames into it. A command-mode panel shows nothing until a DSI controller
 * streams compressed frame commands into it - writes into the splash buffer
 * do NOT reach the glass.
 *
 * Therefore this driver has two paths:
 *   1. splash mode (default): if ABL left the panel in continuous splash
 *      with the DSI controller still streaming (VERIFY on the phone: some
 *      ABLs idle the controller after boot), the frozen splash image stays
 *      on and the kernel console draws into it as on sweet - but frame
 *      UPDATES need path 2.
 *   2. cmd-mode streaming (the real goal, TODO): configure the DSI0
 *      controller + PHY (0xae94000/0xae94400), feed the panel's init
 *      sequence, then stream DSC-compressed frames through the DPU ->
 *      DSI -> panel chain (the DSC hardware block sits in the DPU pipe).
 *      Until this works, iOS boots headless-splash: kernel console visible,
 *      SpringBoard not.
 *
 * MMIO: DPU 0xae01000, DSI0 0xae94000 (mainline sm8250.dtsi). Everything
 * DPU/DSI-specific below is VERIFY-grade bring-up code.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/debug.h>
#include <libkern/libkern.h>

#include "kebab_sm8250.h"

/* osfmk symbols not exported to the BSD layer (same extern as sweet_ufs.c) */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/* DPU MDP_LAYER 0 registers (sm8250 dpu hw catalog, VERIFY offsets) */
#define DPU_SSPP_VIG0_BASE		0x4000
#define SSPP_SRC_FORMAT			0x030	/* VERIFY */
#define SSPP_SRC_SIZE			0x000	/* VERIFY */
#define SSPP_SRC_XY			0x018	/* VERIFY */
#define SSPP_SRC_ADDR			0x010	/* VERIFY */
#define SSPP_OP_MODE			0x03c	/* VERIFY */

#define PANEL_W				SM8250_FB_W
#define PANEL_H				SM8250_FB_H
#define PANEL_STRIDE			SM8250_FB_STRIDE

static volatile uint32_t *dpu;
static volatile uint32_t *splash;	/* ml_io_map of SM8250_FB_BASE */
static uint64_t present_pa;		/* where the display reads its frame */
static int panel_cmd_mode = 1;		/* AMB655X is command mode: no video scanout */

void kebab_panel_init(void)
{
	dpu = (volatile uint32_t *)ml_io_map(SM8250_DPU, 0x1000);
	splash = (volatile uint32_t *)ml_io_map(SM8250_FB_BASE, 0x1700000);
	/*
	 * Do not touch DSI0 during continuous splash (sweet's lesson). Probe
	 * only: report what the DPU says, keep the splash alive.
	 */
	printf("KEBAK PANEL: AMB655X 1080x2400 cmd-mode DSC, splash fb at 0x%x\n"
		"KEBAK PANEL: frame updates need the DSI cmd + DSC stream path "
		"(TODO); splash mode shows the kernel console only\n",
		SM8250_FB_BASE);
}

/*
 * Opt-in hardware scanout (kebabdpu=1). The AMB655X is command mode: there
 * is no video-mode scanout to repoint, so this only logs the state until
 * the DSI+DSC command streaming path exists.
 */
void kebab_panel_dpu_try(void)
{
	if (!dpu) {
		printf("KEBAK PANEL: DPU mmap missing, dpu=1 ignored\n");
		return;
	}
	printf("KEBAK PANEL: kebabdpu=1 but the panel is cmd-mode DSC - "
		"hardware scanout needs the DSI cmd + DSC stream path (TODO)\n");
}

/*
 * Present a frame (a kernel VA of 1080x2400x4 BGRA pixels). Returns the
 * method used:
 *   0 = hardware scanout (needs the DSI cmd/DSC path - not yet)
 *   1 = GPU blit into the splash buffer (visible only while ABL's DSI
 *       controller keeps streaming, or in video-mode reconfigs)
 *   2 = CPU copy into the splash buffer (sweet's path, same caveat)
 */
int kebab_panel_present(const void *frame)
{
	if (!panel_cmd_mode && present_pa && dpu) {
		/* VERIFY: repoint the VIG0 layer address (video-mode only) */
		dpu[(DPU_SSPP_VIG0_BASE + SSPP_SRC_ADDR) / 4] = (uint32_t)present_pa;
		dpu[(DPU_SSPP_VIG0_BASE + SSPP_SRC_ADDR + 4) / 4] = (uint32_t)(present_pa >> 32);
		return 0;
	}
	if (!splash)
		return -1;
	if ((uintptr_t)frame != (uintptr_t)splash) {
		bcopy(frame, (void *)splash, PANEL_H * PANEL_STRIDE);
		return 2;	/* CPU copy; the GPU path joins once the CP ring
				 * proves out (kebab_gpu_blit) */
	}
	return 3;
}
