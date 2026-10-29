/*
 * USB fastboot server for the kebab shim — the hakios xl model.
 *
 * ABL boots the shim through `fastboot boot` of a small image; the phone's
 * USB is already enumerated with the host. The shim adopts the running DWC3
 * (device mode, PHY up, controller halted — the state the probe saw),
 * restarts the core with its own event ring and endpoints, re-enumerates
 * as a fastboot device, and takes the big XNU payload over its own
 * fastboot interface — past ABL's 768 MB download cap:
 *
 *   fastboot oem ramdisk          select the payload slot for the download
 *   fastboot download:4.6GB       (host streams the whole file; no cap here)
 *   fastboot oem run              validate + jump into the payload
 *   fastboot getvar product       -> "kebab-shim" (the resident loader)
 *
 * The DWC3 register model, the DEP 0x10 stride and the EP0 control state
 * machine follow the sweet port's proven driver (xnu patches 0072-0077,
 * bsd/dev/sweet_usb.c); the fastboot command set follows LK's fastboot.
 * The shim runs with the MMU off: every address is physical, memory is
 * Device (not cache coherent), and the DWC3 DMA reads RAM directly.
 */
#include <stdint.h>
#include <string.h>
#include "console.h"
#include "usb.h"

#define rd32(a) ({ uint32_t _v = *(volatile uint32_t *)(a); __asm__ volatile("dsb sy" ::: "memory"); _v; })
#define wr32(a, v) do { *(volatile uint32_t *)(a) = (v); __asm__ volatile("dsb sy" ::: "memory"); } while (0)

#define DCTL_RUN_STOP		(1u << 31)
#define DCTL_CSFTRST		(1u << 30)
#define GCTL_CORESOFTRESET	(1u << 1)
#define DSTS_DEVCTRLHLT		(1u << 22)

#define DEPCMD_SETEPCONFIG	1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL		4
#define DEPCMD_STARTTRANSFER	6
#define DEPCMD_DEPSTARTCFG	9
#define DEPCMD_SETEPCONFIG	1
#define DEPCMD_CMDACT		(1u << 10)

#define TRB_HWO		(1u << 0)
#define TRB_LST		(1u << 1)
#define TRB_ISP_IMI	(1u << 10)
#define TRB_IOC		(1u << 11)
#define TRBCTL_SETUP	(2u << 4)
#define TRBCTL_STATUS2	(3u << 4)
#define TRBCTL_STATUS3	(4u << 4)
#define TRBCTL_DATA	(5u << 4)
#define TRBCTL_NORMAL	(1u << 4)

/* The DMA area lives in the shim's image, which the MMU maps cacheable
 * write-back; the DWC3's DMA is not coherent, so every buffer handed to
 * it is cleaned (writes) or invalidated (reads) over 64-byte lines. */
static void clean_range(void *p, unsigned n)
{
	for (uintptr_t a = (uintptr_t)p; a < (uintptr_t)p + n; a += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(a));
	__asm__ volatile("dsb sy" ::: "memory");
}

static void inv_range(void *p, unsigned n)
{
	for (uintptr_t a = (uintptr_t)p; a < (uintptr_t)p + n; a += 64)
		__asm__ volatile("dc ivac, %0" :: "r"(a));
	__asm__ volatile("dsb sy" ::: "memory");
}

static void ud_delay_ms(unsigned ms)
{
	uint64_t f = 0;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	uint64_t t0 = 0, t1 = 0;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
	for (;;) {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t1));
		if (t1 - t0 >= f / 1000 * ms)
			break;
	}
}

/* The shim links without libc; __builtin_strcmp still emits a call for
 * non-constant strings, so the one string helper lives here. */
static unsigned kstrlen(const char *s)
{
	unsigned n = 0;
	while (*s++)
		n++;
	return n;
}

static void kbcopy(void *d, const void *s, unsigned n)
{
	volatile unsigned char *dd = d;
	const unsigned char *ss = s;
	while (n--)
		*dd++ = *ss++;
}

static int kstrcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static int kmemcmp(const char *a, const char *b, unsigned n)
{
	while (n-- && *a == *b) {
		a++;
		b++;
	}
	return n == (unsigned)-1 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

/* DMA area layout (4 KiB-aligned static buffer; the MMU is off, so its
 * address IS the DMA address). */
static __attribute__((aligned(4096))) struct {
	uint32_t evt[1024];		/* event ring, 4 KiB */
	uint32_t trb0[4];		/* ep0 OUT TRB */
	uint32_t trb1[4];		/* ep0 IN TRB */
	uint32_t trb2[4 * 16];		/* ep1 OUT TRB ring, 16 x 64 KiB */
	uint32_t trb3[4];		/* ep1 IN TRB */
	uint32_t setup[2];		/* 8-byte setup packet */
	uint32_t data[512 / 4];		/* ep0 data stage */
} ud __attribute__((aligned(4096)));

#define RING_TRBS	16
#define RING_TRB_BYTES	(64u << 10)	/* one bulk OUT TRB: 64 KiB */


#define UD_EVT_OFF	0
#define UD_TRB0_OFF	0x1000
#define UD_TRB1_OFF	0x1010
#define UD_TRB2_OFF	0x1020
#define UD_TRB3_OFF	0x1030
#define UD_SETUP_OFF	0x1040
#define UD_DATA_OFF	0x1080
#define UD_END		0x1280

/* The fastboot bulk buffers: one TRB-sized window each way; the download
 * streams into the payload slot at 0x50000000 (below the kernel window). */
#define PAYLOAD_SLOT	0x50000000UL
#define SLOT_END	0x164000000UL	/* slot + 4.6 GB, below the kernel window */
static uint64_t cursor = PAYLOAD_SLOT;	/* where the next bulk OUT data lands */
static uint64_t dl_total, dl_got;
static int dl_active;
static char fb_cmd[256];

static uint32_t evt_pos;
static unsigned ep0_state;
static unsigned ud_events;
enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS };

static void ud_start_trb(unsigned phys_ep, uint32_t trb_off, uint32_t buf_off,
    uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)((uintptr_t)&ud + trb_off);
	uint64_t bpa = (uintptr_t)&ud + buf_off;

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	if (len)
		clean_range((void *)(uintptr_t)bpa, len);
	clean_range((void *)t, 16);
	wr32(DWC3_DEPCMDPAR0(phys_ep), 0);
	wr32(DWC3_DEPCMDPAR1(phys_ep), 0);
	wr32(DWC3_DEPCMDPAR2(phys_ep), 0);
	(void)trb_off;
	uint64_t tpa = (uintptr_t)&ud + trb_off;
	wr32(DWC3_DEPCMDPAR0(phys_ep), (uint32_t)(tpa >> 32));
	wr32(DWC3_DEPCMDPAR1(phys_ep), (uint32_t)tpa);
	wr32(DWC3_DEPCMD(phys_ep), DEPCMD_STARTTRANSFER | DEPCMD_CMDACT);
}

static int ud_epcmd(unsigned phys_ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	wr32(DWC3_DEPCMDPAR0(phys_ep), p0);
	wr32(DWC3_DEPCMDPAR1(phys_ep), p1);
	wr32(DWC3_DEPCMDPAR2(phys_ep), p2);
	wr32(DWC3_DEPCMD(phys_ep), cmd | DEPCMD_CMDACT);
	for (int i = 0; i < 100000; i++) {
		if (!(rd32(DWC3_DEPCMD(phys_ep)) & DEPCMD_CMDACT))
			return 0;
	}
	con_hex2("USB", "epcmd timeout ep", phys_ep, "cmd", cmd & 0xf);
	return -1;
}

/* Arm TRB k of the bulk OUT ring at the cursor (advancing it); k's bytes
 * land at the cursor position when the host sends them. */
static void arm_one(unsigned k)
{
	volatile uint32_t *t = (volatile uint32_t *)&ud.trb2[k * 4];
	uint64_t bpa = cursor;

	if (cursor + RING_TRB_BYTES > SLOT_END)
		return;			/* the slot is full: leave the TRB disarmed */
	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = RING_TRB_BYTES;
	t[3] = TRBCTL_NORMAL | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	clean_range((void *)t, 16);
	uint64_t tpa = (uintptr_t)t;
	wr32(DWC3_DEPCMDPAR0(2), (uint32_t)(tpa >> 32));
	wr32(DWC3_DEPCMDPAR1(2), (uint32_t)tpa);
	wr32(DWC3_DEPCMD(2), DEPCMD_STARTTRANSFER | DEPCMD_CMDACT);
}

static void arm_ring(void)
{
	for (unsigned k = 0; k < RING_TRBS; k++)
		arm_one(k);
}

static void ud_start_setup(void)
{
	ud_start_trb(0, UD_TRB0_OFF, UD_SETUP_OFF, 8, TRBCTL_SETUP);
}

static void ud_stall(void)
{
	ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ud_start_setup();
}

/* Descriptors: the classic Google fastboot identity (VID 0x18D1 /
 * PID 0xD00D), so the host's fastboot client and udev rules take the
 * device instead of the generic bulk driver. */
static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
	0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 250,	/* config: 1 interface */
	9, 4, 0, 0, 1, 0xff, 0x42, 0x03, 0,	/* interface 0, vendor, subclass 0x42, protocol 0x03 (fastboot) */
	7, 5, 0x81, 2, 0x00, 0x02, 0,		/* EP1 IN, bulk, 512 */
	7, 5, 0x01, 2, 0x00, 0x02, 0,		/* EP1 OUT, bulk, 512 */
};
/* No absolute pointers in data (the shim runs unrelocated), so the string
 * descriptors come from a switch. */
static uint32_t ud_string(unsigned idx, uint8_t *out)
{
	const char *str;

	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	/* if-chains (not a switch: gcc turns contiguous switches into a
	 * const pointer table, which is an ABS64 relocation) */
	if (idx == 1)
		str = "kebab-shim";
	else if (idx == 2)
		str = "ios-kebab fastboot loader";
	else if (idx == 3)
		str = "0001";
	else
		return 0;
	uint32_t n = 2;
	for (; *str && n < 126; str++) {
		out[n++] = (uint8_t)*str;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

/* ---- fastboot command set ---- */

static void fb_send(const void *buf, uint32_t len)
{
	volatile uint8_t *dst = (volatile uint8_t *)((uintptr_t)&ud + UD_DATA_OFF);
	/* bulk responses go through the ep1 IN TRB; buffer in the DMA area */
	kbcopy((void *)dst, buf, len);
	ud_start_trb(3, UD_TRB3_OFF, UD_DATA_OFF, len, TRBCTL_NORMAL);
}

static void fb_reply(const char *s)
{
	char b[280];
	uint32_t n = (uint32_t)kstrlen(s);
	kbcopy(b, s, n);
	fb_send(b, n);
}

static void fb_getvar(const char *name)
{
	if (!kstrcmp(name, "product"))
		fb_reply("OKAYkebab-shim");
	else if (!kstrcmp(name, "version"))
		fb_reply("OKAY0.1");
	else if (!kstrcmp(name, "max-download-size"))
		fb_reply("OKAY0x50000000");
	else if (!kstrcmp(name, "result"))
		fb_reply("OKAYrunning");
	else
		fb_reply("FAILunknown variable");
}

static void fb_download(const char *arg)
{
	dl_total = 0;
	for (const char *p = arg; *p; p++)
		dl_total = dl_total * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
	dl_got = 0;
	dl_active = 1;
	cursor = PAYLOAD_SLOT;
	{
		char b[16];
		uint32_t sz = (uint32_t)dl_total;
		for (int i = 0; i < 8; i++)
			b[7 - i] = "0123456789abcdef"[sz >> (i * 4) & 0xf];
		b[8] = 0;
		fb_send("DATA", 4);
		fb_send(b, 8);
	}
	con_hex("USB", dl_total);
}

static void fb_flash(const char *arg)
{
	if (!kstrcmp(arg, "ramdisk") || !kstrcmp(arg, "payload")) {
		if (dl_active && dl_got == dl_total) {
			dl_active = 0;
			fb_reply("OKAY");
		} else {
			fb_reply("FAILno complete download");
		}
	} else {
		fb_reply("FAILunknown partition");
	}
}

/* Validate and jump into the downloaded payload (the loader_boot logic). */
extern const char *loader_boot_at(uintptr_t dtb, const struct payload *p, uint64_t slot);

static void fb_run(void)
{
	if (!dl_got || dl_got != dl_total || dl_total < 0x1000) {
		fb_reply("FAILno payload downloaded");
		return;
	}
	fb_reply("OKAYbooting");
	con_str("USB", "jumping into payload");
	__asm__ volatile("dsb sy" ::: "memory");
	loader_boot_at(0, (const struct payload *)PAYLOAD_SLOT, PAYLOAD_SLOT);
	/* loader_boot_at does not return; if it does, say so */
	fb_reply("FAILxnu returned");
}

static void fb_command(uint32_t len)
{
	fb_cmd[len] = 0;
	con_str("FB", fb_cmd);
	if (!kmemcmp(fb_cmd, "getvar:", 7))
		fb_getvar(fb_cmd + 7);
	else if (!kmemcmp(fb_cmd, "download:", 9))
		fb_download(fb_cmd + 9);
	else if (!kmemcmp(fb_cmd, "flash:", 6))
		fb_flash(fb_cmd + 6);
	else if (!kmemcmp(fb_cmd, "oem ", 4)) {
		if (!kstrcmp(fb_cmd + 4, "ramdisk"))
			fb_reply("OKAY");
		else if (!kstrcmp(fb_cmd + 4, "run"))
			fb_run();
		else
			fb_reply("FAILunknown oem command");
	} else if (!kmemcmp(fb_cmd, "continue", 8) || !kmemcmp(fb_cmd, "boot", 4)) {
		fb_run();
	} else if (!kmemcmp(fb_cmd, "reboot", 6)) {
		fb_reply("OKAY");
	} else {
		fb_reply("FAILunknown command");
	}
}

/* ---- ep0 control transfers ---- */

static void ep0_setup(void)
{
	inv_range((void *)((uintptr_t)&ud + UD_SETUP_OFF), 16);
	const uint8_t *st = (const uint8_t *)((uintptr_t)&ud + UD_SETUP_OFF);
	con_hex2("USB", "setup", st[0] | st[1] << 8, "val", st[2] | st[3] << 8);
	uint8_t *data = (uint8_t *)((uintptr_t)&ud + UD_DATA_OFF);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	int data_in = (type & 0x80) != 0;
	int ok = 1;

	if ((type & 0x60) != 0) {
		ok = 0;
	} else if (req == 6) {		/* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: kbcopy(data, dev_desc, sizeof(dev_desc)); n = sizeof(dev_desc); break;
		case 2: kbcopy(data, cfg_desc, sizeof(cfg_desc)); n = sizeof(cfg_desc); break;
		case 3: n = ud_string(value & 0xff, data); ok = n != 0; break;
		default: ok = 0; break;
		}
	} else if (req == 5) {		/* SET_ADDRESS */
		uint32_t dcfg = rd32(DWC3_DCFG) & ~(0x7fu << 3);
		wr32(DWC3_DCFG, dcfg | ((uint32_t)(value & 0x7f) << 3));
	} else if (req == 9) {		/* SET_CONFIGURATION */
		;			/* the endpoints were configured by DEPSTARTCFG */
	} else if (req == 8) {		/* GET_CONFIGURATION */
		data[0] = 1; n = 1;
	} else if (req == 0) {		/* GET_STATUS */
		data[0] = 0; data[1] = 0; n = 2;
	} else {
		ok = 0;
	}

	if (!ok) {
		ud_stall();
		return;
	}
	if (length == 0) {
		ep0_state = EP0_WAIT_STATUS2;	/* status stage comes on XferNotReady */
		return;
	}
	ep0_state = EP0_DATA;
	if (data_in) {
		if (n > length)
			n = length;
		clean_range((void *)((uintptr_t)&ud + UD_DATA_OFF), n);
		ud_start_trb(1, UD_TRB1_OFF, UD_DATA_OFF, n, TRBCTL_DATA);
	} else {
		ud_start_trb(0, UD_TRB0_OFF, UD_DATA_OFF, 64, TRBCTL_DATA);
	}
}


/* Read-only state report before the server takes over (the ABL handoff). */
void usb_probe(void)
{
	uintptr_t dwc = DWC3_BASE;

	con_str("USB", "adoption probe (ABL state)");
	con_hex("USB", rd32(dwc + DWC3_GSNPSID));
	con_hex("USB", rd32(dwc + DWC3_GCTL));
	con_hex("USB", rd32(dwc + DWC3_GSTS));
	con_hex("USB", rd32(dwc + DWC3_DCFG));
	con_hex("USB", rd32(dwc + DWC3_DCTL));
	con_hex("USB", rd32(dwc + DWC3_DSTS));
	con_hex("USB", rd32(dwc + DWC3_DALEPENA));
	con_str("USB", "probe done (read-only)");
}

/* ---- main server loop ---- */

void usb_server(void)
{
	uintptr_t dwc = DWC3_BASE;

	con_str("USB", "server: adopting the ABL gadget (sweet sequence)");

	/* Device controller soft reset; the PHY and the global setup stay as
	 * ABL left them (the core reset would take the PHY config down too). */
	wr32(dwc + DWC3_DCTL, DCTL_CSFTRST);
	for (int i = 0; i < 1000000 && (rd32(dwc + DWC3_DCTL) & DCTL_CSFTRST); i++)
		;
	if (rd32(dwc + DWC3_DCTL) & DCTL_CSFTRST) {
		con_str("USB", "soft reset STUCK");
		return;
	}

	/* event ring in our DMA area */
	wr32(dwc + DWC3_GEVNTADR_LO, (uint32_t)(uintptr_t)&ud.evt);
	wr32(dwc + DWC3_GEVNTADR_HI, (uint32_t)((uintptr_t)&ud.evt >> 32));
	wr32(dwc + DWC3_GEVNTSIZ, (1u << 31) | 4096);	/* interrupt masked: polled */
	wr32(dwc + DWC3_GEVNTCOUNT, rd32(dwc + DWC3_GEVNTCOUNT) & 0xffff);
	evt_pos = 0;

	/* Toggle the wrapper's session bits (the Linux dwc3-qcom vbus override):
	 * the clear ends the session (the host sees the disconnect), the set
	 * re-validates it and the host re-enumerates with OUR descriptors. */
	uintptr_t qs = 0x0a6f8800UL;
	wr32(qs + 0x30, rd32(qs + 0x30) & ~(1u << 24));	/* LANE0_PWR_PRESENT */
	wr32(qs + 0x10, rd32(qs + 0x10) & ~((1u << 20) | (1u << 28))); /* VBUS + SESSVLD */
	ud_delay_ms(50);
	wr32(qs + 0x30, rd32(qs + 0x30) | (1u << 24));
	wr32(qs + 0x10, rd32(qs + 0x10) | (1u << 20) | (1u << 28));
	ud_delay_ms(50);

	/* High speed, address 0 */
	wr32(dwc + DWC3_DCFG, rd32(dwc + DWC3_DCFG) & ~(7u | (0x7fu << 3)));
	/* Events: disconnect, USB reset, connect done */
	wr32(dwc + DWC3_DEVTEN, 0x7);

	/* Endpoint 0 OUT/IN: start config, control type, 512-byte packets
	 * until the connect-done event (the host then re-programs to 64). */
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (0u << 25), 0) ||
	    ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		con_str("USB", "ep0 setup failed");
		return;
	}
	wr32(dwc + DWC3_DALEPENA, 0x3);
	ep0_state = EP0_SETUP;
	ud_start_setup();

	wr32(dwc + DWC3_DCTL, rd32(dwc + DWC3_DCTL) | DCTL_RUN_STOP);
	for (int i = 0; i < 1000000 && (rd32(dwc + DWC3_DSTS) & DSTS_DEVCTRLHLT); i++)
		;
	con_hex("USB", rd32(dwc + DWC3_DSTS));
	con_str("USB", "server running (fastboot)");

	for (;;) {
		uint32_t cnt = rd32(dwc + DWC3_GEVNTCOUNT);
		while (evt_pos != cnt) {
			volatile uint32_t *e = (volatile uint32_t *)&ud.evt[evt_pos & 0x3fc];
			inv_range((void *)e, 16);
			uint32_t ev = e[0];
			uint32_t trb_len = e[3] >> 16;	/* word 3 = the control word */
			evt_pos += 4;
			wr32(dwc + DWC3_GEVNTCOUNT, evt_pos & 0xffff);

			if (ev & 1) {	/* device event (the sweet decode) */
				uint32_t dtype = (ev >> 8) & 0xf;
				if (dtype == 1) {
					con_str("USB", "bus reset");
					wr32(dwc + DWC3_DCFG, rd32(dwc + DWC3_DCFG) & ~(0x7fu << 3));
				} else if (dtype == 2) {
					con_str("USB", "connect done");
					ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
					    (1u << 8) | (1u << 10) | (0u << 25), 0);
					ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
					    (1u << 8) | (1u << 10) | (1u << 25), 0);
					ud_epcmd(2, DEPCMD_DEPSTARTCFG, 1, 2, 0);
					ud_epcmd(2, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3),
					    (1u << 8) | (1u << 10) | (0u << 25), 0);
					ud_epcmd(3, DEPCMD_DEPSTARTCFG, 1, 2, 1);
					ud_epcmd(3, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3),
					    (1u << 8) | (1u << 10) | (1u << 25), 0);
					wr32(dwc + DWC3_DALEPENA, 0xf);
					arm_ring();
				} else if (dtype == 0) {
					con_str("USB", "disconnect");
				}
				continue;
			}
			uint32_t phys_ep = (ev >> 1) & 0x1f;
			uint32_t evt_type = (ev >> 6) & 0xf;
			uint32_t evt_status = (ev >> 12) & 0xf;
			if (ud_events++ < 60) {
				if (evt_type == 1)
					con_hex2("USB", "XC ep", phys_ep, "len", trb_len);
				else if (evt_type == 3)
					con_hex2("USB", "NR ep", phys_ep, "st", evt_status);
				else if (evt_type == 4)
					con_hex2("USB", "XIP ep", phys_ep, "st", evt_status);
			}

			if (evt_type == 3 && evt_status == 2) {	/* XferNotReady, control status */
				if (ep0_state == EP0_WAIT_STATUS2 || ep0_state == EP0_WAIT_STATUS3) {
					uint32_t ctl = ep0_state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
					ep0_state = EP0_STATUS;
					ud_start_trb(phys_ep, phys_ep ? UD_TRB1_OFF : UD_TRB0_OFF,
					    UD_DATA_OFF, 0, ctl);
				}
				continue;
			}
			if (evt_type == 4 && phys_ep == 0) {	/* XferNotReady: arm the setup */
				ud_start_setup();
				continue;
			}
			if (evt_type != 1)	/* not XferComplete */
				continue;
			if (phys_ep == 0 || phys_ep == 1) {
				if (ep0_state == EP0_SETUP) {
					ep0_setup();
				} else if (ep0_state == EP0_DATA) {
					ep0_state = EP0_WAIT_STATUS3;
				} else if (ep0_state == EP0_STATUS) {
					ep0_state = EP0_SETUP;
					ud_start_setup();
				}
			} else if (phys_ep == 2) {	/* ep1 OUT chunk */
				uint32_t k = (e[1] - (uint32_t)(uintptr_t)&ud.trb2) / 16;
				if (trb_len) {
					if (dl_active) {
						dl_got += trb_len;
						cursor += trb_len;
						if (dl_got >= dl_total) {
							dl_active = 0;
							fb_reply("OKAY");
						}
					} else {
						const uint8_t *src = (const uint8_t *)((uintptr_t)&ud.trb2[k]);
						uint32_t n = trb_len < sizeof(fb_cmd) - 1 ? trb_len : sizeof(fb_cmd) - 1;
						kbcopy(fb_cmd, src, n);
						fb_command(n);
					}
				}
				arm_one(k);
			} else if (phys_ep == 3) {	/* ep1 IN done */
				;
			}
		}
	}
}
