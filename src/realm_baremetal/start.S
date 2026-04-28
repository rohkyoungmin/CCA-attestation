#define REALM_MARKER_BASE 0x80010000

	.section .text.boot, "ax"
	.align 11
	.global _start
_start:
	b	entry
	.long	0
	.quad	0			/* text_offset */
	.quad	_image_end - _start	/* image_size */
	.quad	2			/* flags: 4K pages, little-endian */
	.quad	0
	.quad	0
	.quad	0
	.long	0x644d5241		/* "ARMd" */
	.long	0

entry:
	ldr	x0, =REALM_MARKER_BASE

	mov	w1, #'B'
	strb	w1, [x0, #0]
	mov	w1, #'M'
	strb	w1, [x0, #1]
	mov	w1, #'R'
	strb	w1, [x0, #2]
	mov	w1, #'K'
	strb	w1, [x0, #3]

	mov	w1, #0x11
	strb	w1, [x0, #4]
	mov	w1, #0x22
	strb	w1, [x0, #5]
	mov	w1, #0x33
	strb	w1, [x0, #6]
	mov	w1, #0x44
	strb	w1, [x0, #7]

spin:
	ldrb	w1, [x0, #16]
	add	w1, w1, #1
	strb	w1, [x0, #16]
	b	spin

	.align 12
_image_end:
