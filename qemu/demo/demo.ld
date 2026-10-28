/*
 * Link the demo at 0x40000188: tools/mkmacho.py places the ELF's LOAD bytes
 * at Mach-O file offset 0x188 (right after the Mach-O header), so the link
 * VAs equal the Mach-O VAs exactly. The shim then copies the segment to
 * physBase + (vmaddr - virtBase); delta is page-aligned, so every PC-relative
 * adrp resolves correctly at runtime. No .bss (nothing zeroes it).
 */
ENTRY(_entry)
PHDRS { text PT_LOAD FILEHDR PHDRS FLAGS(7); }
SECTIONS
{
	. = 0x40000188;
	.text : { *(.text.head) *(.text*) } :text
	.rodata : { *(.rodata*) *(.rodata.*) }
	.data : { *(.data*) }
	. = ALIGN(0x1000);
}
