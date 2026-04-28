ENTRY(_start)

SECTIONS
{
	. = 0x80000000;

	.text : ALIGN(0x1000) {
		KEEP(*(.text.boot))
		*(.text*)
	}

	.rodata : ALIGN(0x1000) {
		*(.rodata*)
	}

	.data : ALIGN(0x1000) {
		*(.data*)
	}

	.bss (NOLOAD) : ALIGN(0x1000) {
		*(.bss*)
		*(COMMON)
	}

	/DISCARD/ : {
		*(.comment*)
		*(.note*)
		*(.eh_frame*)
	}
}
