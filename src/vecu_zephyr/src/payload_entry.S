#include <zephyr/toolchain.h>

.section .text.realm_payload_entry, "ax", %progbits

.global realm_payload_entry
.type realm_payload_entry, %function

.extern __start
.extern __data_start
.extern _kernel
.extern __reset_prep_c
.extern z_arm64_el_highest_init
.extern z_arm64_el1_init
.extern z_bss_zero
.extern z_arm64_interrupt_init
.extern z_cstart
.extern realm_payload_mmu_probe
.extern realm_payload_enable_mmu_final
.extern realm_payload_status_shared_addr
.extern realm_payload_shared_alias_bit
.extern realm_payload_dtb_addr

.equ REALM_SHIM_HANDOFF_BASE,   0x801ff000
.equ REALM_SHIM_HANDOFF_MAGIC,  0x524d484f
.equ REALM_STATUS_MAGIC,        0x524d5354
.equ REALM_PAYLOAD_PHASE_STUB,  0x19
.equ REALM_PAYLOAD_PHASE_RESET_PREP, 0x1a
.equ REALM_PAYLOAD_PHASE_RESET_PREP_DONE, 0x1b
.equ REALM_PAYLOAD_PHASE_HIGHEST_INIT_DONE, 0x1c
.equ REALM_PAYLOAD_PHASE_EL1_INIT_DONE, 0x1d
.equ REALM_PAYLOAD_PHASE_Z_PREP_C, 0x1e
.equ REALM_PAYLOAD_PHASE_UNEXPECTED_EL, 0x1f
.equ REALM_PAYLOAD_PHASE_BSS_ZERO_DONE, 0x22
.equ REALM_PAYLOAD_PHASE_TLBI_PROBE_BEFORE, 0x26
.equ REALM_PAYLOAD_PHASE_TLBI_PROBE_AFTER, 0x27
.equ REALM_PAYLOAD_PHASE_MMU_INIT_DONE, 0x23
.equ REALM_PAYLOAD_PHASE_INTERRUPT_INIT_DONE, 0x24
.equ REALM_PAYLOAD_PHASE_CSTART, 0x25
.equ REALM_PAYLOAD_PHASE_PRE_FINAL_MMU_ENABLE, 0x33
.equ REALM_PAYLOAD_PHASE_RUNTIME_CTX_READY, 0x3b

.macro spin_delay count
	mov	x28, #\count
990:
	subs	x28, x28, #1
	b.ne	990b
.endm

.macro publish_phase phase
	cbz	x27, 991f
	ldr	w28, =REALM_STATUS_MAGIC
	str	w28, [x27]
	mov	w28, #\phase
	str	w28, [x27, #4]
	str	xzr, [x27, #16]
	str	xzr, [x27, #24]
	str	xzr, [x27, #32]
991:
.endm

realm_payload_entry:
	mov	x20, x0

	ldr	x21, =REALM_SHIM_HANDOFF_BASE
	ldr	w22, [x21]
	ldr	w23, =REALM_SHIM_HANDOFF_MAGIC
	mov	x27, xzr
	cmp	w22, w23
	b.ne	1f

	ldr	x23, [x21, #16]
	ldr	x27, [x21, #24]
	cbz	x27, 1f

	publish_phase REALM_PAYLOAD_PHASE_STUB
	ldr	x28, =realm_payload_entry
	str	x28, [x27, #16]
	ldr	x28, =__start
	str	x28, [x27, #24]

1:
	publish_phase REALM_PAYLOAD_PHASE_RESET_PREP
	ldr	x24, =__data_start
	ldr	x25, =z_prep_c
	mov	x0, x20
	bl	__reset_prep_c

	publish_phase REALM_PAYLOAD_PHASE_RESET_PREP_DONE
	bl	z_arm64_el_highest_init

	publish_phase REALM_PAYLOAD_PHASE_HIGHEST_INIT_DONE
	mrs	x0, currentel
	cmp	x0, #0x4
	b.eq	2f

	publish_phase REALM_PAYLOAD_PHASE_UNEXPECTED_EL
	cbz	x27, 9f
	str	x0, [x27, #16]
	b	9f

2:
	bl	z_arm64_el1_init
	publish_phase REALM_PAYLOAD_PHASE_EL1_INIT_DONE
	msr	spsel, #0x1
	msr	daifclr, #0x4
	isb
	publish_phase REALM_PAYLOAD_PHASE_Z_PREP_C

	ldr	x0, =_kernel
	msr	tpidrro_el0, x0
	bl	z_bss_zero
	publish_phase REALM_PAYLOAD_PHASE_BSS_ZERO_DONE

	/*
	 * BSS is clear now, so re-read and export the shim handoff values
	 * before enabling the final MMU. Re-reading here avoids relying on
	 * early-init helper calls preserving x23/x27, while still avoiding a
	 * post-MMU access to the shim handoff window.
	 */
	ldr	x21, =REALM_SHIM_HANDOFF_BASE
	ldr	w22, [x21]
	ldr	w28, =REALM_SHIM_HANDOFF_MAGIC
	cmp	w22, w28
	b.ne	3f
	ldr	x23, [x21, #16]
	ldr	x27, [x21, #24]
	ldr	x20, [x21, #32]

3:
	ldr	x28, =realm_payload_status_shared_addr
	str	x27, [x28]
	ldr	x28, =realm_payload_shared_alias_bit
	str	x23, [x28]
	ldr	x28, =realm_payload_dtb_addr
	str	x20, [x28]

	publish_phase REALM_PAYLOAD_PHASE_TLBI_PROBE_BEFORE
	dsb	ishst
	tlbi	vmalle1
	dsb	ish
	isb
	publish_phase REALM_PAYLOAD_PHASE_TLBI_PROBE_AFTER

	mov	x0, x27
	bl	realm_payload_mmu_probe

	publish_phase REALM_PAYLOAD_PHASE_PRE_FINAL_MMU_ENABLE
	spin_delay 0x800000

	mov	x0, x27
	bl	realm_payload_enable_mmu_final
	publish_phase REALM_PAYLOAD_PHASE_MMU_INIT_DONE

	bl	z_arm64_interrupt_init
	publish_phase REALM_PAYLOAD_PHASE_INTERRUPT_INIT_DONE
	publish_phase REALM_PAYLOAD_PHASE_RUNTIME_CTX_READY

	publish_phase REALM_PAYLOAD_PHASE_CSTART
	b	z_cstart

9:
	b	9b

.size realm_payload_entry, . - realm_payload_entry
