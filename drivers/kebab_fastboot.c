/*
 * ios-kebab: fastboot server inside XNU (the two-stage no-flash phone boot).
 *
 * The 8T ABL caps fastboot downloads at 768 MB, so the 4.6 GB root payload
 * cannot ride in the boot image. The shim boots XNU with a SMALL payload
 * (kernel + ADT, the RAMDisk region reserved but empty); this driver brings
 * up the DWC3 in device mode (the register sequence proven by the sweet
 * port's ECM driver, patches 0072-0077) with FASTBOOT descriptors instead
 * of CDC-ECM, serves the fastboot command set on the bulk endpoints, and
 * streams `download:` data straight into the /chosen/memory-map/RAMDisk
 * region. kebab_init() (the SweetPlatformExpert hook) waits here until the
 * download completes or the boot-arg kebabwait=0 says not to; the root
 * mount that follows finds the streamed HFS image.
 *
 *   fastboot boot shim-boot.img          (the small resident image)
 *   fastboot flash ramdisk root.img      (the 4.3 GB stream; no ABL cap)
 *   fastboot continue                    (kebab_init proceeds, launchd boots)
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/kern_event.h>
#include <kern/debug.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>

#include "kebab_sm8250.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern vm_offset_t ml_static_ptovirt(vm_offset_t);

#define DWC3_BASE	SM8250_USB1_DWC3	/* 0x0a600000 */
#define GCTL		0xc110
#define GEVNTADRLO	0xc400
#define GEVNTADRHI	0xc404
#define GEVNTSIZ	0xc408
#define GEVNTCOUNT	0xc40c
#define DCFG		0xc700
#define DCTL		0xc704
#define DEVTEN		0xc708
#define DSTS		0xc70c
#define DALEPENA	0xc720
#define DEPCMDPAR2(n)	(0xc800 + 0x10 * (n) + 0x00)
#define DEPCMDPAR1(n)	(0xc800 + 0x10 * (n) + 0x04)
#define DEPCMDPAR0(n)	(0xc800 + 0x10 * (n) + 0x08)
#define DEPCMD(n)	(0xc800 + 0x10 * (n) + 0x0c)

#define DCTL_RUN_STOP	(1u << 31)
#define DCTL_CSFTRST	(1u << 30)
#define DSTS_DEVCTRLHLT (1u << 22)

#define DEPCMD_SETEPCONFIG	1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL		4
#define DEPCMD_STARTTRANSFER	6
#define DEPCMD_DEPSTARTCFG	9
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

/* physical endpoints: ep0 OUT=0, ep0 IN=1, bulk OUT=2 (the download), bulk IN=3 */
#define EP_RX		2
#define EP_TX		3

/* DMA area layout (one contiguous 16K allocation) */
#define UD_EVT		0x0000
#define UD_EVT_SIZE	0x1000
#define UD_TRB_OUT	0x1000
#define UD_TRB_IN	0x1010
#define UD_TRB_RX	0x1020
#define UD_TRB_TX	0x1030
#define UD_SETUP	0x1040
#define UD_DATA		0x1100
#define UD_RX		0x1400		/* the bulk OUT window: 64 KiB */
#define UD_FRAME_MAX	(64u << 10)
#define UD_SIZE		0x11400

static struct {
	vm_offset_t dwc;
	vm_offset_t dma;
	uint64_t    dma_pa;
	uint32_t    evt_pos;
	unsigned    state;		/* ep0 control state machine */
	uint32_t    data_in, data_len;
	int         configured;
	int         download_active;
	uint64_t    dl_got, dl_total;
	uint64_t    rd_pa, rd_size;	/* the RAMDisk region (the md0) */
	char        fb_cmd[256];
} ud;

enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS };

static inline uint32_t
dwc_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(ud.dwc + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static inline void
dwc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(ud.dwc + off) = v;
	__asm__ volatile ("dmb oshst" ::: "memory");
}

static void
ud_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(ud.dma_pa + off), len, TRUE);
}

static int
ud_epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	for (int i = 0; i < 1000; i++) {
		uint32_t v = dwc_rd(DEPCMD(ep));
		if (!(v & DEPCMD_CMDACT)) {
			uint32_t status = (v >> 12) & 0xf;
			if (status) {
				printf("KEBAK FB: ep%u cmd 0x%x status %u\n", ep, cmd & 0xf, status);
				return -1;
			}
			return 0;
		}
		delay(1);
	}
	printf("KEBAK FB: ep%u cmd 0x%x timeout\n", ep, cmd & 0xf);
	return -1;
}

static void
ud_start_trb(unsigned ep, uint32_t trb, uint32_t buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + trb);
	uint64_t bpa = ud.dma_pa + buf;

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	ud_sync(trb, 16);
	if (len)
		ud_sync(buf, len);
	uint64_t tpa = ud.dma_pa + trb;
	ud_epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0);
}

static void
ud_start_setup(void)
{
	ud.state = EP0_SETUP;
	ud_start_trb(0, UD_TRB_OUT, UD_SETUP, 8, TRBCTL_SETUP);
}

/* The fastboot descriptors: Google's classic identity. */
static const uint8_t fb_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
	0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t fb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 250,
	9, 4, 0, 0, 1, 0xff, 0x42, 0x03, 0,
	7, 5, 0x81, 2, 0x00, 0x02, 0,
	7, 5, 0x01, 2, 0x00, 0x02, 0,
};

static uint32_t
fb_string(unsigned idx, uint8_t *out)
{
	const char *str;

	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
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

/* The bulk endpoints, at SET_CONFIGURATION(1) (the sweet ECM pattern). */
static void
fb_configure(void)
{
	if (ud.configured)
		return;
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((uint32_t)EP_RX << 25), 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("KEBAK FB: bulk endpoint setup failed\n");
		return;
	}
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) | (1u << EP_RX) | (1u << EP_TX));
	ud.configured = 1;
	ud_start_trb(EP_RX, UD_TRB_RX, UD_RX, UD_FRAME_MAX, TRBCTL_NORMAL);
	printf("KEBAK FB: bulk endpoints configured\n");
}

static void
fb_reply(const char *s)
{
	size_t n = strlen(s);
	memcpy((void *)(ud.dma + UD_DATA), s, n);
	ud_start_trb(EP_TX, UD_TRB_TX, UD_DATA, (uint32_t)n, TRBCTL_NORMAL);
}

static void
fb_download(const char *arg)
{
	ud.dl_total = 0;
	for (const char *p = arg; *p; p++)
		ud.dl_total = ud.dl_total * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
	ud.dl_got = 0;
	ud.download_active = 1;
	printf("KEBAK FB: download %llu bytes into 0x%llx\n", ud.dl_total, ud.rd_pa);
	char b[16];
	uint32_t sz = (uint32_t)ud.dl_total;
	for (int i = 0; i < 8; i++)
		b[7 - i] = "0123456789abcdef"[sz >> (i * 4) & 0xf];
	b[8] = 0;
	memcpy((void *)(ud.dma + UD_DATA), "DATA", 4);
	ud_sync(UD_DATA, 4);
	ud_start_trb(EP_TX, UD_TRB_TX, UD_DATA, 4, TRBCTL_NORMAL);
	memcpy((void *)(ud.dma + UD_DATA), b, 8);
	ud_sync(UD_DATA, 8);
	ud_start_trb(EP_TX, UD_TRB_TX, UD_DATA, 8, TRBCTL_NORMAL);
}

static void
fb_command(uint32_t len)
{
	ud.fb_cmd[len] = 0;
	printf("KEBAK FB: %s\n", ud.fb_cmd);
	if (!memcmp(ud.fb_cmd, "getvar:", 7)) {
		const char *v = ud.fb_cmd + 7;
		if (!strcmp(v, "product"))
			fb_reply("OKAYkebab-shim");
		else if (!strcmp(v, "version"))
			fb_reply("OKAY0.1");
		else if (!strcmp(v, "max-download-size"))
			fb_reply("OKAY0x20000000");
		else if (!strcmp(v, "result"))
			fb_reply("OKAYrunning");
		else
			fb_reply("FAILunknown variable");
	} else if (!memcmp(ud.fb_cmd, "download:", 9)) {
		fb_download(ud.fb_cmd + 9);
	} else if (!memcmp(ud.fb_cmd, "flash:", 6)) {
		if (!strcmp(ud.fb_cmd + 6, "ramdisk") || !strcmp(ud.fb_cmd + 6, "payload")) {
			if (ud.download_active || ud.dl_got != ud.dl_total)
				fb_reply("FAILno complete download");
			else
				fb_reply("OKAY");
		} else {
			fb_reply("FAILunknown partition");
		}
	} else if (!memcmp(ud.fb_cmd, "oem ", 4)) {
		if (!strcmp(ud.fb_cmd + 4, "ramdisk"))
			fb_reply("OKAY");
		else if (!strcmp(ud.fb_cmd + 4, "run"))
			fb_reply("OKAY");
		else
			fb_reply("FAILunknown oem command");
	} else if (!memcmp(ud.fb_cmd, "continue", 8) || !memcmp(ud.fb_cmd, "boot", 4)) {
		fb_reply("OKAY");
	} else {
		fb_reply("FAILunknown command");
	}
}

/* The control transfer handler (the sweet's proven state machine). */
static void
fb_setup(void)
{
	ud_sync(UD_SETUP, 8);
	const uint8_t *st = (const uint8_t *)(ud.dma + UD_SETUP);
	uint8_t *data = (uint8_t *)(ud.dma + UD_DATA);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	int ok = 1;

	ud.data_in = (type & 0x80) != 0;
	if ((type & 0x60) != 0) {
		ok = 0;
	} else if (req == 6) {
		switch (value >> 8) {
		case 1: memcpy(data, fb_dev_desc, sizeof(fb_dev_desc)); n = sizeof(fb_dev_desc); break;
		case 2: memcpy(data, fb_cfg_desc, sizeof(fb_cfg_desc)); n = sizeof(fb_cfg_desc); break;
		case 3: n = fb_string(value & 0xff, data); ok = n != 0; break;
		default: ok = 0; break;
		}
	} else if (req == 5) {
		uint32_t dcfg = dwc_rd(DCFG) & ~(0x7fu << 3);
		dwc_wr(DCFG, dcfg | ((uint32_t)(value & 0x7f) << 3));
	} else if (req == 9) {
		if (value == 1)
			fb_configure();
	} else if (req == 8) {
		data[0] = ud.configured ? 1 : 0; n = 1;
	} else if (req == 0) {
		data[0] = 0; data[1] = 0; n = 2;
	} else if (req == 11 || req == 1 || req == 3) {
	} else {
		ok = 0;
	}
	if (!ok) {
		ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
		ud_start_setup();
		return;
	}
	if (length == 0) {
		ud.state = EP0_WAIT_STATUS2;
		return;
	}
	ud.state = EP0_DATA;
	ud.data_len = n > length ? length : n;
	/* the data TRB is armed on the XferNotReady */
}

static void
fb_rx_done(uint32_t len)
{
	if (!len)
		return;
	if (ud.download_active) {
		/* stream the chunk into the RAMDisk region */
		ud_sync(UD_RX, UD_FRAME_MAX);
		memcpy((void *)ml_static_ptovirt(ud.rd_pa + ud.dl_got), (void *)(ud.dma + UD_RX), len);
		ud.dl_got += len;
		if (ud.dl_got >= ud.dl_total) {
			ud.download_active = 0;
			printf("KEBAK FB: download complete (%llu bytes)\n", ud.dl_got);
			fb_reply("OKAY");
		}
	} else {
		uint32_t n = len < sizeof(ud.fb_cmd) - 1 ? len : sizeof(ud.fb_cmd) - 1;
		memcpy(ud.fb_cmd, (void *)(ud.dma + UD_RX), n);
		fb_command(n);
	}
}

static void
fb_event(uint32_t ev)
{
	if (ev & 1) {
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {
			printf("KEBAK FB: bus reset\n");
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
		} else if (type == 2) {
			printf("KEBAK FB: connect done\n");
			ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			    (1u << 8) | (1u << 10) | (0u << 25), 0);
			ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			    (1u << 8) | (1u << 10) | (1u << 25), 0);
		} else if (type == 0) {
			printf("KEBAK FB: disconnect\n");
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f;
	unsigned type = (ev >> 6) & 0xf;
	unsigned status = (ev >> 12) & 0xf;

	if (type == 1) {	/* XferComplete */
		if (ep == 0 || ep == 1) {
			if (ud.state == EP0_SETUP)
				fb_setup();
			else if (ud.state == EP0_DATA)
				ud.state = EP0_WAIT_STATUS3;
			else if (ud.state == EP0_STATUS)
				ud_start_setup();
		} else if (ep == EP_RX) {
			volatile uint32_t *t = (volatile uint32_t *)(ud.dma + UD_TRB_RX);
			ud_sync(UD_TRB_RX, 16);
			uint32_t len = UD_FRAME_MAX - (t[2] & 0xffffff);
			fb_rx_done(len);
			ud_start_trb(EP_RX, UD_TRB_RX, UD_RX, UD_FRAME_MAX, TRBCTL_NORMAL);
		}
	} else if (type == 3 && status == 2) {	/* XferNotReady, control status */
		if (ud.state == EP0_WAIT_STATUS2 || ud.state == EP0_WAIT_STATUS3) {
			uint32_t ctl = ud.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			ud.state = EP0_STATUS;
			ud_start_trb(ep, ep ? UD_TRB_IN : UD_TRB_OUT, UD_DATA, 0, ctl);
		}
	} else if (type == 3 && status == 1) {	/* XferNotReady, data */
		if (ud.state == EP0_DATA) {
			if (ud.data_in) {
				ud_sync(UD_DATA, ud.data_len);
				ud_start_trb(1, UD_TRB_IN, UD_DATA, ud.data_len, TRBCTL_DATA);
			} else {
				ud_start_trb(0, UD_TRB_OUT, UD_DATA, 64, TRBCTL_DATA);
			}
		}
	}
}

static void
fb_thread(void *arg __unused, wait_result_t wr __unused)
{
	for (;;) {
		uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
		if (count) {
			ud_sync(UD_EVT, UD_EVT_SIZE);
			for (uint32_t done = 0; done < count; done += 4) {
				uint32_t ev = *(volatile uint32_t *)(ud.dma + UD_EVT + ud.evt_pos);
				ud.evt_pos = (ud.evt_pos + 4) % UD_EVT_SIZE;
				fb_event(ev);
			}
			dwc_wr(GEVNTCOUNT, count);
		}
		delay(1);
	}
}

/* The RAMDisk region from /chosen/memory-map (the md0 the root mount uses). */
static void
fb_find_ramdisk(void)
{
	DTEntry entry;
	uintptr_t const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(0, "/chosen/memory-map", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "RAMDisk", (void const **)&prop, &size) != kSuccess ||
	    size < 16) {
		printf("KEBAK FB: no RAMDisk region in the ADT\n");
		return;
	}
	ud.rd_pa = prop[0];
	ud.rd_size = prop[1];
	printf("KEBAK FB: RAMDisk region 0x%llx + 0x%llx\n", ud.rd_pa, ud.rd_size);
}

int
kebab_fastboot_done(void)
{
	return !ud.download_active && ud.dl_total != 0 && ud.dl_got == ud.dl_total;
}

void
kebab_fastboot_start(void)
{
	/* the wrapper session bits, like the sweet vbus override */
	uintptr_t qs = ml_io_map(SM8250_USB1_WRAPPER, 0x400);
	if (qs == 0) {
		printf("KEBAK FB: wrapper map failed\n");
		return;
	}

	ud.dwc = ml_io_map(DWC3_BASE, 0x10000);
	if (ud.dwc == 0) {
		printf("KEBAK FB: DWC3 map failed\n");
		return;
	}
	if (kmem_alloc_contig(kernel_map, &ud.dma, UD_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("KEBAK FB: no DMA memory\n");
		return;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)ud.dma);
	if (!pn)
		return;
	ud.dma_pa = ptoa_64(pn);
	fb_find_ramdisk();

	/* the proven device-mode sequence (the sweet ECM driver) */
	dwc_wr(DCTL, DCTL_CSFTRST);
	for (int i = 0; i < 500 && (dwc_rd(DCTL) & DCTL_CSFTRST); i++)
		delay(1000);
	if (dwc_rd(DCTL) & DCTL_CSFTRST) {
		printf("KEBAK FB: soft reset stuck\n");
		return;
	}
	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma_pa + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma_pa + UD_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	ud.evt_pos = 0;

	uint32_t dcfg = dwc_rd(DCFG) & ~(7u | (0x7fu << 3));
	dwc_wr(DCFG, dcfg | 0);
	dwc_wr(DEVTEN, 0x7);

	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (0u << 25), 0) ||
	    ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("KEBAK FB: ep0 setup failed\n");
		return;
	}
	dwc_wr(DALEPENA, 0x3);
	ud_start_setup();

	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	for (int i = 0; i < 500 && (dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++)
		delay(1000);

	thread_t thread;
	if (kernel_thread_start((thread_continue_t)fb_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("KEBAK FB: cannot start the event thread\n");
		return;
	}
	thread_deallocate(thread);
	printf("KEBAK FB: fastboot server running (waiting for the host)\n");
}
