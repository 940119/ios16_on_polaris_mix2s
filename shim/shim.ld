ENTRY(_head)
SECTIONS
{
	. = 0;
	/*
	 * Keep .rodata inside .text: a separate output section made objcopy
	 * shift it 68 bytes away from its VMA, so every string and font lookup
	 * read the wrong bytes at runtime.
	 */
	.text : { *(.text.head) *(.text*) *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. += 0x8000;
	. = ALIGN(16);
	_stack_top = .;
	. = ALIGN(4096);
	_sec_stacks = .;
	. += 8 * 0x1000;
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
