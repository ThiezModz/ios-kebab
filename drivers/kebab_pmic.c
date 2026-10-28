/*
 * ios-kebab: OnePlus 8T keys + PMIC driver (SPMI PON block, PM8150B).
 *
 * Same scheme as sweet_keys.c (SPMI arbiter observer reads -> SweetHIDButtons
 * consumer keys): the PMIC PON block latches power and volume-down presses,
 * volume-up is a PMIC GPIO routed through the PON block. All SIDs/addresses
 * here are SM8250 values from kebab_sm8250.h and marked VERIFY: the first
 * boot with 'kebabkeys=1' prints every SPMI register this driver touches,
 * and the ADT in tools/mkadt.py carries the same numbers.
 */
#include <kern/debug.h>
#include <kern/thread.h>

#include "kebab_sm8250.h"

extern int sweet_spmi_read(int sid, uint32_t addr, uint32_t *out);	/* sweet_keys.c helper, VERIFY */
extern void sweet_hid_key(int key, int down);	/* 0 power, 1 vol+, 2 vol- */

#define PON_INT_RT_STS		(SM8250_PMIC_PON_BASE + 0x10)	/* VERIFY */

static uint32_t pon_prev;
static thread_t kebab_keys_thr;

static void kebab_keys_thread(void *unused, wait_result_t wait);

static void kebab_keys_poll(void)
{
	uint32_t sts = 0;
	if (sweet_spmi_read(SM8250_PMIC_PON_SID, PON_INT_RT_STS, &sts))
		return;
	uint32_t delta = sts ^ pon_prev;
	pon_prev = sts;
	if (delta & 0x1)	/* KPDPWR = power, VERIFY bit */
		sweet_hid_key(0, sts & 0x1);
	if (delta & 0x2)	/* RESIN = volume down, VERIFY bit */
		sweet_hid_key(2, sts & 0x2);
	if (delta & 0x4)	/* volume up GPIO latch, VERIFY bit */
		sweet_hid_key(1, sts & 0x4);
}

/*
 * Battery: the 8T reports charge through the PM8150B FG over SPMI. Until the
 * FG register map is confirmed on-device, publish the same static
 * IOPMPowerSource sweet used (SweetBattery); the readout hook below is
 * prepared and prints raw FG data with 'kebakbatt=1'.
 */
static void kebab_battery_probe(void)
{
	uint32_t v = 0;
	if (sweet_spmi_read(SM8250_PMIC_PON_SID, 0x1000 /* FG_BATT_SOC, VERIFY */, &v) == 0)
		kprintf("KEBAK PMIC: fuel gauge raw 0x%08x (VERIFY register)\n", v);
}

void kebab_pmic_init(void)
{
	if (sweet_spmi_read(SM8250_PMIC_PON_SID, PON_INT_RT_STS, &pon_prev))
		kprintf("KEBAK PMIC: PON block unreadable (SPMI arbiter not up?)\n");
	else
		kprintf("KEBAK PMIC: PON block readable, polling keys\n");
	kebab_battery_probe();
	/* poll thread, same pattern as sweet_keys.c */
	kernel_thread_start((thread_continue_t)kebab_keys_thread, NULL, &kebab_keys_thr);
	thread_deallocate(kebab_keys_thr);
}

static void kebab_keys_thread(void *unused, wait_result_t wait)
{
	(void)unused;
	(void)wait;
	kprintf("KEBAK PMIC: keys poll thread up\n");
	for (;;) {
		kebab_keys_poll();
		delay(2);	/* ~20 ms at hz=100, like the sweet poll threads */
	}
}
