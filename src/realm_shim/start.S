#include "contract.h"

#define RSI_FID(n)                     (0xC4000000UL | (n))
#define RSI_VERSION                    RSI_FID(0x190)
#define RSI_REALM_CONFIG               RSI_FID(0x196)
#define RSI_IPA_STATE_SET              RSI_FID(0x197)

#define RSI_SUCCESS                    0
#define RSI_RIPAS_EMPTY                0

#define HANDOFF_MAGIC_OFF             0
#define HANDOFF_VERSION_OFF           4
#define HANDOFF_STATUS_GPA_OFF        8
#define HANDOFF_SHARED_ALIAS_OFF      16
#define HANDOFF_STATUS_SHARED_OFF     24
#define HANDOFF_DTB_OFF               32
#define HANDOFF_PAYLOAD_LOAD_OFF      40
#define HANDOFF_PAYLOAD_ENTRY_OFF     48

	.section .text.boot, "ax"
	.align 11
	.global _start
_start:
	b	shim_entry
	.long	0
	.quad	0
	.quad	_image_end - _start
	.quad	2
	.quad	0
	.quad	0
	.quad	0
	.long	0x644d5241
	.long	0

shim_entry:
	/* Preserve DTB pointer from kvmtool contract. */
	mov	x20, x0

	/* RSI_VERSION */
	ldr	x0, =RSI_VERSION
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	mov	x4, xzr
	mov	x5, xzr
	mov	x6, xzr
	mov	x7, xzr
	smc	#0

	/* RSI_REALM_CONFIG into local buffer */
	adrp	x21, realm_cfg
	add	x21, x21, :lo12:realm_cfg
	ldr	x0, =RSI_REALM_CONFIG
	mov	x1, x21
	mov	x2, xzr
	mov	x3, xzr
	mov	x4, xzr
	mov	x5, xzr
	mov	x6, xzr
	mov	x7, xzr
	smc	#0
	cmp	x0, #RSI_SUCCESS
	b.ne	shim_hang

	ldr	x22, [x21]              /* ipa_bits */
	mov	x23, #1
	sub	x24, x22, #1
	lsl	x23, x23, x24           /* shared alias bit */

	/* Mark status page shared */
	ldr	x0, =RSI_IPA_STATE_SET
	ldr	x1, =REALM_SHIM_STATUS_PAGE_BASE
	mov	x2, #REALM_SHIM_STATUS_PAGE_SIZE
	mov	x3, #RSI_RIPAS_EMPTY
	mov	x4, xzr
	mov	x5, xzr
	mov	x6, xzr
	mov	x7, xzr
	smc	#0
	cmp	x0, #RSI_SUCCESS
	b.ne	shim_hang

	ldr	x25, =REALM_SHIM_STATUS_PAGE_BASE
	orr	x25, x25, x23

	/* Private handoff page for the payload app. */
	ldr	x27, =REALM_SHIM_HANDOFF_BASE
	ldr	w0, =REALM_SHIM_HANDOFF_MAGIC
	str	w0, [x27, #HANDOFF_MAGIC_OFF]
	mov	w0, #REALM_SHIM_HANDOFF_VERSION
	str	w0, [x27, #HANDOFF_VERSION_OFF]
	ldr	x0, =REALM_SHIM_STATUS_PAGE_BASE
	str	x0, [x27, #HANDOFF_STATUS_GPA_OFF]
	str	x23, [x27, #HANDOFF_SHARED_ALIAS_OFF]
	str	x25, [x27, #HANDOFF_STATUS_SHARED_OFF]
	str	x20, [x27, #HANDOFF_DTB_OFF]
	ldr	x0, =REALM_SHIM_PAYLOAD_LOAD_BASE
	str	x0, [x27, #HANDOFF_PAYLOAD_LOAD_OFF]
	ldr	x0, =REALM_SHIM_PAYLOAD_ENTRY
	str	x0, [x27, #HANDOFF_PAYLOAD_ENTRY_OFF]

	/* phase: entry */
	ldr	w0, =REALM_SHIM_STATUS_MAGIC
	str	w0, [x25, #0]
	mov	w0, #REALM_SHIM_PHASE_ENTRY
	str	w0, [x25, #4]

	/* phase: rsi_ready, value0 = ipa_bits, value1 = shared alias bit */
	ldr	w0, =REALM_SHIM_STATUS_MAGIC
	str	w0, [x25, #0]
	mov	w0, #REALM_SHIM_PHASE_RSI_READY
	str	w0, [x25, #4]
	str	x22, [x25, #16]
	str	x23, [x25, #24]

	/* phase: payload_jump */
	ldr	w0, =REALM_SHIM_STATUS_MAGIC
	str	w0, [x25, #0]
	mov	w0, #REALM_SHIM_PHASE_PAYLOAD_JUMP
	str	w0, [x25, #4]

	/* Jump to the future payload contract entry. */
	mov	x0, x20
	ldr	x26, =REALM_SHIM_PAYLOAD_ENTRY
	br	x26

shim_hang:
	b	shim_hang

	.align 12
realm_cfg:
	.quad	0

	.align 12
_image_end:
