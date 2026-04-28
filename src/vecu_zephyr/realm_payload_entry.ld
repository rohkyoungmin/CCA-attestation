SECTION_PROLOGUE(.realm_payload_entry,,)
{
	. = ALIGN(4);
	KEEP(*(.text.realm_payload_entry))
} GROUP_ROM_LINK_IN(RAMABLE_REGION, ROMABLE_REGION)
