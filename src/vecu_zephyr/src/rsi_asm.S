/*
 * rsi_asm.S - ARM CCA RSI attestation init (8-argument SMC)
 *
 * rsi_attest_init_smc(c0, c1, c2, c3, c4, c5, c6, c7, *out_size)
 *   x0 = FID (RSI_ATTESTATION_TOKEN_INIT)
 *   x1..x7 = challenge[0..6]
 *   x8 = challenge[7]   (caller passes via stack since ABI only has x0-x7)
 *   x9 = pointer to out_size (passed as 9th arg on stack)
 *
 * On return: x0 = status, x1 = upper bound of token size
 *
 * Calling convention (AAPCS64):
 *   x0-x7  = first 8 integer arguments
 *   x8+    = spilled to stack
 *   x8 here is used as indirect result location register in AAPCS
 *   but for our custom asm call we treat x8 as the 9th SMC argument
 *
 * We pass: c0-c6 in x0-x6 (7 args), c7 and out_size on stack.
 * This function:
 *   1. Loads c7 from stack[0], out_size ptr from stack[1]
 *   2. Sets x0=FID, shifts c0-c6 into x1-x7, c7 into x8
 *   3. Issues SMC #0
 *   4. Stores x1 -> *out_size
 *   5. Returns x0
 */

.global rsi_attest_init_smc
.type   rsi_attest_init_smc, %function
rsi_attest_init_smc:
    /* x0=fid, x1=c0, x2=c1, x3=c2, x4=c3, x5=c4, x6=c5, x7=c6 */
    /* [sp+0]=c7, [sp+8]=out_size ptr                              */
    stp  x8,  x9,  [sp, #-16]!  /* save callee-saved regs        */

    ldr  x8,  [sp, #16]          /* c7 (first stack arg)          */
    ldr  x9,  [sp, #24]          /* out_size ptr (second stack arg)*/

    /* Shift: x1=c0..x7=c6, x8=c7 already set                     */
    smc  #0

    /* x0 = status, x1 = upper_bound                              */
    cbz  x9, 1f
    str  x1, [x9]                /* *out_size = x1                */
1:
    ldp  x8,  x9,  [sp],  #16    /* restore                       */
    ret
.size rsi_attest_init_smc, . - rsi_attest_init_smc
