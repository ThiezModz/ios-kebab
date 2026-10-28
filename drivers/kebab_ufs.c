/*
 * ios-kebab: OnePlus 8T UFS driver bring-up glue (SM8250).
 *
 * sweet_ufs.c owns the UFS host (JEDEC UFSHC, same IP on SM8250); this file
 * adds the SM8250 specifics: the GCC clock votes ABL already left on (the
 * sweet port observed the same for USB), the UFS PHY state, and the kebab
 * init entry point that every other kebab driver hangs off.
 *
 * With 'kebabnoprobe=1' none of the kebab drivers start - useful to bisect
 * a boot regression to the new device code.
 */
#include <kern/debug.h>
#include <pexpert/pexpert.h>	/* PE_parse_boot_argn */

#include "kebab_sm8250.h"

extern void kebab_gpu_init(void);
extern void kebab_panel_init(void);
extern void kebab_panel_dpu_try(void);
extern void kebab_touch_init(void);
extern void kebab_pmic_init(void);
extern void kebab_usb_init(void);
extern void kebab_audio_init(void);
extern void kebab_ufs_clocks(void);

/*
 * Called from bsd/dev/sweet_ufs.c right after the UFS root is up (the 0082
 * patch adds the call next to sweet_keys_init/sweet_touch_init). Each
 * subsystem behind a boot-arg stays off until proven on the phone; defaults
 * match what sweet enabled by default.
 */
void kebab_init(void)
{
	uint64_t skip = 0;
	if (PE_parse_boot_argn("kebabnoprobe", &skip, sizeof(skip)) && skip)
		return;

	kprintf("KEBAK: OnePlus 8T (SM8250) device bring-up\n");
	kebab_ufs_clocks();		/* report what ABL left on */
	kebab_panel_init();
	kebab_pmic_init();
	kebab_touch_init();
	kebab_gpu_init();
	uint64_t dpu = 0;
	if (PE_parse_boot_argn("kebabdpu", &dpu, sizeof(dpu)) && dpu)
		kebab_panel_dpu_try();
	uint64_t usb = 0;
	if (PE_parse_boot_argn("kebakusb", &usb, sizeof(usb)) && usb)
		kebab_usb_init();
	uint64_t audio = 0;
	if (PE_parse_boot_argn("kebakaudio", &audio, sizeof(audio)) && audio)
		kebab_audio_init();
}

/* SM8250 UFS clocks: report the GCC enables ABL left on; enable if off. */
void kebab_ufs_clocks(void)
{
	volatile uint32_t *gcc = (volatile uint32_t *)ml_io_map(SM8250_GCC, 0x100000);
	if (!gcc) {
		kprintf("KEBAK UFS: GCC mmap failed\n");
		return;
	}
	/* GCC clock enable registers hold bit 0 = root enabled (VERIFY offsets) */
	uint32_t ufs_phy = gcc[SM8250_GCC_UFS_PHY_AXI / 4];
	uint32_t ufs_ice = gcc[SM8250_GCC_UFS_ICE_CORE / 4];
	kprintf("KEBAK UFS: phy_axi 0x%08x ice_core 0x%08x (VERIFY offsets)\n",
		ufs_phy, ufs_ice);
	if (!(ufs_phy & 1))
		gcc[SM8250_GCC_UFS_PHY_AXI / 4] = ufs_phy | 1;
	if (!(ufs_ice & 1))
		gcc[SM8250_GCC_UFS_ICE_CORE / 4] = ufs_ice | 1;
}
