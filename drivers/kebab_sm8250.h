/*
 * ios-kebab: OnePlus 8T (kebab) / Qualcomm SM8250 hardware map.
 *
 * One header so every driver shares the same addresses and there is a single
 * place to fix them when the live device tree disagrees. Sources: mainline
 * sm8250.dtsi / sm8250-gcc.h, OnePlusOSS/android_kernel_oneplus_sm8250 kebab
 * dts (kona-mtp.dtsi, dsi-panel-oplus20828-samsung-amb655x), and the live
 * FDT on the phone (tools/mkadt.py feeds the same numbers to the Apple
 * Device Tree). Registers marked VERIFY need a phone boot or the live FDT
 * to confirm.
 */
#ifndef KEBAK_SM8250_H
#define KEBAK_SM8250_H

/* ---- SoC basics ---- */
#define SM8250_GICD			0x17a00000UL
#define SM8250_GICR			0x17a60000UL
#define SM8250_TIMEBASE			19200000UL	/* CXO, like all SM8xxx */
#define SM8250_VTIMER_INTID		19		/* Group-1 IRQ on Qualcomm */
#define SM8250_IMEM			0x14680000UL	/* restart reason @ 0x65c */
#define SM8250_IMEM_RESTART		0x146aa65cUL
#define SM8250_PSHOLD			0x0c264000UL

/* ---- Kryo 585 CPU topology (1x A77 2.84 GHz + 3x A77 2.42 GHz + 4x A55 1.8 GHz)
 * MPIDR: Aff1 = core index 0..7, as on every Qualcomm 8xx. XNU sees one
 * homogeneous SMP cluster (the sweet port proved the scheme). ---- */
#define SM8250_NR_CPUS			8
#define SM8250_A77_PRIME_HZ		2841600000ULL	/* cpu7, VERIFY binning */
#define SM8250_A77_HZ			2419200000ULL
#define SM8250_A55_HZ			1804800000ULL
#define SM8250_L3_HZ			1344000000ULL

/* ---- GPU: Adreno 650 (mainline sm8250.dtsi) ---- */
#define SM8250_GPU_CC			0x3d90000UL	/* gpucc, 0x9000 */
#define SM8250_GPU_BASE			0x3d00000UL	/* adreno 650 core, 0x40000 */
#define SM8250_GMU_BASE			0x3d6a000UL	/* GMU block, 0x30000 */
#define SM8250_GPU_SMMU			0x3da0000UL	/* for the SMMU-bypass work */
/* GMU firmware on the vendor partition (OnePlusOS OOS builds) */
#define SM8250_GMU_FW_PATH		"/vendor/firmware/a650_gmu.bin"
#define SM8250_ZAP_FW_PATH		"/vendor/firmware/a650_zap.mbn"	/* VERIFY */

/* ---- Display: DPU + DSI0 (mainline sm8250.dtsi) ---- */
#define SM8250_MDSS			0xae00000UL
#define SM8250_DPU			0xae01000UL
#define SM8250_DPU_VBIF			0xaeb0000UL
#define SM8250_DSI0			0xae94000UL
#define SM8250_DSI0_PHY		0xae94400UL

/* ---- UFS (mainline sm8250.dtsi) ---- */
#define SM8250_UFS_HC			0x01d84000UL	/* ufs_mem_hc */
#define SM8250_UFS_HC_SIZE		0x3000UL
#define SM8250_UFS_PHY			0x01d87000UL
#define SM8250_UFS_PHY_SIZE		0x1000UL
#define SM8250_UFS_ICE			0x01d90000UL	/* unused: no encryption */

/* ---- USB: DWC3 dual-role, QMP phy (the sweet ECM patches drive the same
 * controller; the wrapper/phy addresses are SM8250-specific, VERIFY) ---- */
#define SM8250_USB1_WRAPPER		0x0a6f8000UL	/* QSCRATCH @ +0x8800, VERIFY */
#define SM8250_USB1_DWC3		0x0a600000UL
#define SM8250_USB1_QMP_PHY		0x088e9000UL
#define SM8250_USB1_HS_PHY		0x088e3000UL

/* ---- GENI I2C (QUP): the touch controller is Synaptics S3908 on
 * qupv3_se13_i2c @ 0xa94000 (kona-mtp.dtsi). QUPv3 wrappers on SM8250:
 * 0x8c0000 / 0x9c0000 / 0xac0000, SEs at 0x4000 strides. ---- */
#define SM8250_QUPV3_0			0x8c0000UL
#define SM8250_QUPV3_1			0x9c0000UL
#define SM8250_QUPV3_2			0xac0000UL
#define SM8250_TOUCH_I2C_BASE		0x0a94000UL	/* SE13, from kona-mtp.dtsi */
#define SM8250_TOUCH_I2C_ADDR		0x4b		/* Synaptics S3908 */
#define SM8250_TOUCH_IRQ_GPIO		39		/* VERIFY: TLMM number */
#define SM8250_TOUCH_RESET_GPIO		38		/* active low, VERIFY */

/* ---- SPMI + PMIC (kona-pmic-overlay.dtsi) ---- */
#define SM8250_SPMI_ARB			0x0c440000UL
#define SM8250_SPMI_ARB_SIZE		0x8000UL
/* USIDs: PM8150 0/1, PM8150B 2/3, PM8150L 4/5, PM8009 0xa/0xb */
#define SM8250_PMIC_PON_SID		0		/* PM8150 PON, VERIFY */
#define SM8250_PMIC_PON_BASE		0x800		/* PON block, VERIFY */
#define SM8250_PON_KPDPWR		0x810		/* VERIFY */
#define SM8250_PON_RESIN		0x811		/* VERIFY */
/* touch rails (kona-mtp.dtsi): vdd 2.8 V = pm8150_l13, vcc 1.8 V = pm8150a_l1 */
#define SM8250_TOUCH_LDO_2V8		13		/* PM8150 LDO13, VERIFY SID */
#define SM8250_TOUCH_LDO_1V8		1		/* PM8150A LDO1, VERIFY SID */

/* ---- Clocks (gcc@100000 on SM8250); offsets from gcc-sm8250.c ---- */
#define SM8250_GCC			0x00100000UL
#define SM8250_GCC_UFS_BCR		0x77000UL
#define SM8250_GCC_UFS_GDSC		0x77004UL
#define SM8250_GCC_UFS_PHY_AXI		0x77010UL	/* branch */
#define SM8250_GCC_UFS_ICE_CORE		0x77064UL	/* branch */

/* ---- ABL splash framebuffer (kebab: VERIFY address + stride) ---- */
#define SM8250_FB_BASE			0x9c000000UL
#define SM8250_FB_W			1080
#define SM8250_FB_H			2400
#define SM8250_FB_STRIDE		(SM8250_FB_W * 4)

#endif /* KEBAK_SM8250_H */
