/*
 * ios-kebab: OnePlus 8T keys + PMIC driver (SPMI PON block, PM8150).
 *
 * Same scheme as sweet_keys.c (SPMI arbiter observer reads -> SweetHIDButtons
 * consumer keys): the PM8150 PON block latches power (PON1 KPDPWR) and
 * volume-down (PON2 RESIN); volume-up is a PM8150 GPIO (mainline kebab:
 * GPIO6, active-low). The sweet SPMI lanes are keyed to the SM7150 PM6150
 * block map, so the kebab PON lane setup is the first wiring step; until
 * then this driver reports and the sweet keys driver stays off.
 *
 * Battery: the 8T's real fuel gauge is a TI BQ28Z610 on qupv3_se16_i2c
 * (0x888000, addr 0x55, oplus bq27541 wrapper - kona-mtp.dtsi); the PM8150B
 * SPMI fg-gen4 is disabled by default. Until the GENI SE16 read path is
 * proven, the sweet static IOPMPowerSource (SweetBattery) stays published.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>		/* delay */
#include <kern/debug.h>
#include <kern/thread.h>

#include "kebab_sm8250.h"

/* bsd/dev/sweet_keys.c (patch 0060): HID button emission */
extern void sweet_hid_key(int key, int down);	/* 0 power, 1 vol+, 2 vol- */

static thread_t kebab_keys_thr;

static void kebab_keys_thread(void *unused, wait_result_t wait)
{
	(void)unused;
	(void)wait;
	printf("KEBAK PMIC: keys poll thread up (PON lane not wired yet; "
		"buttons stay dead until the SPMI arbiter lane setup lands)\n");
	/*
	 * TODO (first wiring step): program the SPMI arbiter (0xc440000) the
	 * way sweet_keys.c does for SM7150, with the kebab PM8150 PON map
	 * (SID 0, block 0x800, KPDPWR/RESIN at 0x810/0x811 VERIFY), then poll
	 * and call sweet_hid_key(0/2, ...) plus the PM8150 GPIO6 volume-up
	 * lane. Every value is in kebab_sm8250.h.
	 */
	for (;;)
		delay(10);	/* ~100 ms */
}

void kebab_pmic_init(void)
{
	printf("KEBAK PMIC: PM8150 SID0 PON @ 0x800 (KPDPWR/RESIN), BQ28Z610 "
		"gauge on SE16 @ 0x888000 - both need their bus lane setup\n");
	kernel_thread_start((thread_continue_t)kebab_keys_thread, NULL, &kebab_keys_thr);
	thread_deallocate(kebab_keys_thr);
}
