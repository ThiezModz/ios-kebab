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
 * The S3908 speaks a different register map than sweet's Goodix GT9896, so
 * this driver implements the Synaptics read flow itself and reuses only the
 * HID emission part of sweet_touch.c. 'kebabtouch=1' runs a probe dump at
 * boot (chip id + config revision) without starting the event thread.
 */
#include <kern/debug.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

#include "kebab_sm8250.h"

/* Synaptics S3908 essentials (downstream synaptics_dsx driver, VERIFY) */
#define S3908_CMD_IDENTIFY		0x04	/* RMI f01 query base, VERIFY */
#define S3908_F01_PRODUCT_INFO		0x10	/* VERIFY */

extern int sweet_rpmh_ldo_vote(int enable);		/* bsd/dev/sweet_rpmh.c */
extern void sweet_hid_touch(int finger, int down, int x, int y);	/* HID emission */
extern int sweet_geni_i2c_se(int base, int se);		/* GENI SE setup helper, VERIFY name */
extern int sweet_geni_i2c_read(uint32_t addr, uint8_t reg, uint8_t *buf, int len);	/* VERIFY */

static void kebab_touch_probe(void)
{
	uint8_t id[8];
	if (sweet_geni_i2c_read(SM8250_TOUCH_I2C_ADDR, S3908_CMD_IDENTIFY, id, sizeof(id)) != 0) {
		kprintf("KEBAK TOUCH: S3908 @ 0x%x on SE13 does not answer "
			"(rail not voted? reset GPIO held?)\n", SM8250_TOUCH_I2C_ADDR);
		return;
	}
	kprintf("KEBAK TOUCH: S3908 id %02x%02x%02x%02x %02x%02x%02x%02x (VERIFY layout)\n",
		id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

void kebab_touch_init(void)
{
	/* 1. Power the touch IC: both rails through RPMh. */
	if (sweet_rpmh_ldo_vote(1) != 0)
		kprintf("KEBAK TOUCH: RPMh vote failed; controller may be unpowered\n");
	else
		kprintf("KEBAK TOUCH: touch rails (pm8150_l13 + pm8150a_l1) voted on\n");

	/* 2. Reset sequence: reset GPIO38 is ACTIVE-HIGH (dt flag 0x1):
	 * assert (high) 5 ms, deassert (low), 60 ms settle. */
	extern int sweet_gpio_out(int gpio, int value);	/* TLMM helper, VERIFY name */
	sweet_gpio_out(SM8250_TOUCH_RESET_GPIO, 1);
	delay(1);
	sweet_gpio_out(SM8250_TOUCH_RESET_GPIO, 0);
	delay(6);

	/* 3. Bring up GENI SE13 for I2C (the sweet GENI init generalized). */
	if (sweet_geni_i2c_se(SM8250_TOUCH_I2C_BASE, 13) != 0) {
		kprintf("KEBAK TOUCH: GENI SE13 init failed\n");
		return;
	}
	kebab_touch_probe();

	/* 4. The finger-event thread: poll the S3908 reports (RMI f11/f12,
	 * VERIFY registers) and emit HID digitizer events exactly like
	 * sweet_touch.c does. kebabtouch=0 skips the thread but still probes. */
	uint64_t no_thread = 0;
	if (PE_parse_boot_argn("kebabtouch", &no_thread, sizeof(no_thread)) && no_thread == 0)
		return;
	kprintf("KEBAK TOUCH: event thread NOT started (report polling is the "
		"next bring-up step; kebabtouch=1 starts it once the map is verified)\n");
}
