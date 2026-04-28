#ifndef REALM_SHIM_CONTRACT_H
#define REALM_SHIM_CONTRACT_H

#define REALM_SHIM_LOAD_BASE          0x80000000UL
#define REALM_SHIM_STATUS_PAGE_BASE   0x83e10000UL
#define REALM_SHIM_STATUS_PAGE_SIZE   0x1000UL
#define REALM_SHIM_STATUS_MAGIC       0x524d5354UL
#define REALM_SHIM_HANDOFF_BASE       0x801ff000UL
#define REALM_SHIM_HANDOFF_MAGIC      0x524d484fUL
#define REALM_SHIM_HANDOFF_VERSION    1UL

/*
 * Initial fixed payload handoff contract.
 * The shim occupies the first 2 MiB of Realm RAM and hands off to a payload
 * that is linked to execute from REALM_SHIM_PAYLOAD_LOAD_BASE.
 */
#define REALM_SHIM_PAYLOAD_OFFSET     0x00200000UL
#define REALM_SHIM_PAYLOAD_LOAD_BASE  (REALM_SHIM_LOAD_BASE + REALM_SHIM_PAYLOAD_OFFSET)
#ifndef REALM_SHIM_PAYLOAD_ENTRY
#define REALM_SHIM_PAYLOAD_ENTRY      REALM_SHIM_PAYLOAD_LOAD_BASE
#endif

#define REALM_SHIM_PHASE_ENTRY        0x10UL
#define REALM_SHIM_PHASE_RSI_READY    0x11UL
#define REALM_SHIM_PHASE_PAYLOAD_JUMP 0x12UL

#ifndef __ASSEMBLER__
struct realm_shim_handoff {
	unsigned int magic;
	unsigned int version;
	unsigned long status_gpa;
	unsigned long shared_alias_bit;
	unsigned long status_shared_addr;
	unsigned long dtb;
	unsigned long payload_load_base;
	unsigned long payload_entry;
};
#endif

#endif /* REALM_SHIM_CONTRACT_H */
