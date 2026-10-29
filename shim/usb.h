/* USB adoption probe + fastboot server for the kebab shim (see usb.c). */
#ifndef SHIM_USB_H
#define SHIM_USB_H

#include <stdint.h>

/* DWC3 global and device register offsets (Synopsys DWC3, as on sweet). */
#define DWC3_BASE		0x0a600000UL
#define DWC3_GSNPSID		0xc120
#define DWC3_GCTL		0xc110
#define DWC3_GSTS		0xc118
#define DWC3_GUSB2PHYCFG	0xc200
#define DWC3_DCFG		0xc700
#define DWC3_DCTL		0xc704
#define DWC3_DEVTEN		0xc708
#define DWC3_DSTS		0xc70c
#define DWC3_DALEPENA		0xc720
#define DWC3_GEVNTADR_LO	0xc400
#define DWC3_GEVNTADR_HI	0xc404
#define DWC3_GEVNTSIZ		0xc408
#define DWC3_GEVNTCOUNT		0xc40c
#define DWC3_GHWPARAMS0		0xc140
/* The DEP register stride is 0x10 on this SoC family (proven on sweet). */
#define DWC3_DEP_BASE(n)	(0xc800 + 0x10 * (n))
#define DWC3_DEPCMDPAR2(n)	(DWC3_DEP_BASE(n) + 0x00)
#define DWC3_DEPCMDPAR1(n)	(DWC3_DEP_BASE(n) + 0x04)
#define DWC3_DEPCMDPAR0(n)	(DWC3_DEP_BASE(n) + 0x08)
#define DWC3_DEPCMD(n)		(DWC3_DEP_BASE(n) + 0x0c)
#define DWC3_DEPST(n)		(DWC3_DEP_BASE(n) + 0x10)

/* GSNPSID values seen on the sweet/kebab DWC3 (v3.20a and around). */
#define DWC3_ID_320A		0x5533320a

struct payload;

void usb_probe(void);
void usb_server(void);
const char *loader_boot_at(uintptr_t dtb, const struct payload *p, uint64_t slot);

#endif
