/*
 * ios-kebab: OnePlus 8T audio stub (WCD938x codec + NXP TFA9874 amplifiers).
 *
 * Stub-grade on purpose: the sweet port runs without audio (mediaserverd
 * starts without a HAL device; the VirtualAudio plugin is disabled in the
 * root image). This file only (1) holds the codec in reset so it cannot
 * pull the audio rails, and (2) documents what a real driver needs:
 *   - WCD938x (kona-audio-overlay.dtsi: "qcom,wcd938x-codec", split codec,
 *     reset GPIO32, supplies cdc-vdd-rxtx/vddio/buck = S4A 1.8 V, mic-bias
 *     BOB 3.3-3.6 V), data over SoundWire RX/TX slaves
 *   - 2x NXP TFA9874 speaker amps on qupv3_se15_i2c (0x884000), addrs
 *     0x34/0x35, reset GPIOs 91/99, supply pm8150_s4, firmware
 *     /odm/firmware/tfa98xx.cnt
 *   - then a /product/audio node so CoreAudio sees a device (the sweet
 *     notes document this path)
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/debug.h>

#include "kebab_sm8250.h"

void kebab_audio_init(void)
{
	kprintf("KEBAK AUDIO: WCD938x held in reset, TFA9874 amps untouched; no "
		"audio device published (matches sweet: mediaserverd runs with "
		"the VirtualAudio plugin disabled)\n");
}
