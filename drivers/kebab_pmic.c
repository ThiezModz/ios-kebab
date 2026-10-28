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
	/* PM8150 PON: PON1 = KPDPWR (power), PON2 = RESIN (volume down).
	 * Volume up is a PM8150 GPIO (mainline kebab: GPIO6, active-low);
	 * poll it through the SPMI GPIO status once the sweet SPMI helper
	 * covers GPIOs (VERIFY on first boot). */
	if (delta & 0x1)	/* KPDPWR, VERIFY bit */
		sweet_hid_key(0, sts & 0x1);
	if (delta & 0x2)	/* RESIN, VERIFY bit */
		sweet_hid_key(2, sts & 0x2);
}

/*
 * Battery: the 8T's real fuel gauge is a TI BQ28Z610 on qupv3_se16_i2c
 * (0x888000, addr 0x55, oplus bq27541 wrapper — kona-mtp.dtsi); the PM8150B
 * SPMI fg-gen4 is disabled by default. Until the I2C read path is proven,
 * the sweet static IOPMPowerSource (SweetBattery) stays published; this
 * probe prints raw gauge data with 'kebakbatt=1'.
 */
static void kebab_battery_probe(void)
{
	extern int sweet_geni_i2c_read(uint32_t addr, uint8_t reg, uint8_t *buf, int len);	/* VERIFY */
	uint8_t v[2];
	if (sweet_geni_i2c_read(SM8250_GAUGE_I2C_ADDR, 0x2c /* BQ28Z610 StateOfCharge, VERIFY */,
				v, sizeof(v)) == 0)
		kprintf("KEBAK PMIC: BQ28Z610 gauge raw %02x%02x (VERIFY register)\n", v[0], v[1]);
	else
		kprintf("KEBAK PMIC: BQ28Z610 gauge does not answer on SE16 (expected: "
			"needs the GENI SE init first)\n");
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
