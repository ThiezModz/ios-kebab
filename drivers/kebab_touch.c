/*
 * ios-kebab: OnePlus 8T touchscreen driver (GENI I2C + Synaptics S3908).
 *
 * The sweet port's touch chain (HID digitizer events -> SweetHIDTouch ->
 * backboardd) is reused unchanged; this file is the kebab front end:
 *   - controller: Synaptics S3908 @ 0x4b on qupv3_se13_i2c (0xa94000),
 *     16 TX x 36 RX, coordinates 1079x2399 (kona-mtp.dtsi)
 *   - power: vdd 2.8 V = pm8150_l13, vcc 1.8 V = pm8150a_l1, both through
 *     RPMh votes (the sweet port showed the IC never answers unpowered)
 *   - reset GPIO38 active-HIGH (dt flag 0x1), IRQ GPIO39
 *
 * The S3908 speaks a different register map than sweet's Goodix GT9896, and
 * the sweet GENI code drives SE7 (0xa84000) where kebab needs SE13. The
 * GENI SE13 setup + the S3908 read flow are the next wiring step (a
 * parameterized sweet_touch.c); this file votes the rails and reports.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>		/* delay */
#include <kern/debug.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

#include "kebab_sm8250.h"

/* bsd/dev/sweet_rpmh.c (patch 0062): RPMh voltage vote by resource name */
extern int sweet_rpmh_vreg_enable(const char *res, unsigned mv);
/* bsd/dev/sweet_touch.c (patch 0060): Goodix protocol + HID emission */
extern void sweet_touch_init(void);

void kebab_touch_init(void)
{
	/*
	 * 1. Power the touch IC through RPMh. Resource names for the kebab
	 *    PMICs (pm8150_l13 = "ldoa13"?, pm8150a_l1 = "ldoc1"?) are VERIFY
	 *    against the kebab DT rpmh-regulator node on the first boot; the
	 *    sweet port used "ldoc11" for its PM6150L L11C.
	 */
	int e1 = sweet_rpmh_vreg_enable("ldoa13", 3008);	/* VERIFY name */
	int e2 = sweet_rpmh_vreg_enable("ldoc1", 1800);		/* VERIFY name */
	if (e1 || e2)
		kprintf("KEBAK TOUCH: RPMh vote failed (%d/%d); S3908 likely "
			"unpowered until the names are corrected\n", e1, e2);
	else
		kprintf("KEBAK TOUCH: touch rails (pm8150_l13 + pm8150a_l1) voted\n");

	/*
	 * 2. GENI SE13 + S3908 probe + event thread: NOT wired yet (the sweet
	 *    GENI front end hardcodes SE7; it needs a bus/address parameter
	 *    pass, plus the Synaptics register map). Until then the HID chain
	 *    stays quiet - iOS boots without touch, exactly like sweet before
	 *    its 0060/0063 patches.
	 */
	kprintf("KEBAK TOUCH: SE13/S3908 bring-up TODO; sweet_touch_init() not "
		"called (it drives the wrong SE on kebab)\n");
}
