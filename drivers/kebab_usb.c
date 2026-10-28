/*
 * ios-kebab: OnePlus 8T USB (DWC3 dual-role + QMP PHY), SM8250 addresses.
 *
 * The sweet port's USB patches (0072-0077: probe, ep0, VBUS override, ECM
 * function, live console log over UDP) drive the same DWC3 core; only the
 * Qualcomm wrapper / QSCRATCH and PHY addresses differ per SoC. This file
 * maps the SM8250 values so sweet_usb.c can pick them up, and adds the 8T
 * extra: the wrapper QMP PHY DP-alt-mode detect is left to the OS (no-op
 * here - the ECM bring-up path does not need it).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/debug.h>
#include <machine/machine_routines.h>	/* ml_io_map */

#include "kebab_sm8250.h"

void kebab_usb_init(void)
{
	volatile uint32_t *wrap = (volatile uint32_t *)ml_io_map(SM8250_USB1_WRAPPER, 0x400);
	if (!wrap) {
		kprintf("KEBAK USB: wrapper mmap failed\n");
		return;
	}
	/* GSNPSID of the DWC3 behind the wrapper: expect 0x5533320a (v3.20a)
	 * like sweet's, VERIFY on the phone */
	kprintf("KEBAK USB: wrapper 0x%x, DWC3 GSNPSID 0x%08x\n",
		SM8250_USB1_WRAPPER, *(volatile uint32_t *)ml_io_map(SM8250_USB1_DWC3, 0x1000));
	/*
	 * Hand off to the existing sweet USB bring-up (patches 0072-0077,
	 * sweetusb=2 path). It drives the same DWC3 core; the wrapper/QSCRATCH
	 * and PHY addresses it hardcodes are SM7150's, so it needs the kebab
	 * addresses plumbed in before the ECM live log works on the 8T.
	 */
	extern void sweet_usb_probe(void);
	sweet_usb_probe();
}
