/*
 * ios-kebab demo kernel entry. The shim jumps here with:
 *   x0 = boot_args physical address, MMU + D-cache + I-cache all OFF.
 * Position-independent; the loader copied the whole segment, so all we
 * need is a stack somewhere in the kernel's RAM: boot_args + 32K.
 */
	.section .text.head, "ax"
	.globl _entry
_entry:
	add	sp, x0, #0x8000
	b	demo_main		/* x0 = boot_args */
