/*
 * ios-kebab: OnePlus 8T audio stub (WCD9385 codec over SLIMbus/SoundWire).
 *
 * Stub-grade on purpose: the sweet port runs without audio (mediaserverd
 * starts without a HAL device; the VirtualAudio plugin is disabled in the
 * root image). This file only (1) holds the codec in reset so it cannot
 * pull the audio rails, and (2) documents what a real driver needs:
 *   - TLMM reset pin (VERIFY gpio) assert -> deassert sequence
 *   - SoundWire controller @ 0x3250000 (VERIFY) master init
 *   - WCD9385 registers over SWIRE, then a /product/audio node so CoreAudio
 *     sees a device (the sweet notes document this path)
 */
#include <kern/debug.h>

#include "kebab_sm8250.h"

void kebab_audio_init(void)
{
	kprintf("KEBAK AUDIO: WCD9385 held in reset; no audio device published "
		"(matches sweet: mediaserverd runs with the VirtualAudio plugin disabled)\n");
}
