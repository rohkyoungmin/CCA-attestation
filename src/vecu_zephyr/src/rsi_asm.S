/*
 * rsi_asm.S - ARM CCA RSI attestation init (8-argument SMC)
 *
 * rsi_attest_init_smc(fid, token_buf_ipa, c0, c1, c2, c3, c4, c5, c6, c7)
 *   x0 = FID (RSI_ATTESTATION_TOKEN_INIT)
 *   x1 = token buffer IPA
 *   x2..x7 = challenge[0..5]
 *   x8 = challenge[6]
 *   x9 = challenge[7]
 *
 * On return: x0 = status, x1 = upper bound of token size
 *
 * Calling convention (AAPCS64):
 *   x0-x7  = first 8 integer arguments
 *   x8+    = spilled to stack
 *   x8 here is used as indirect result location register in AAPCS
 *   but for our custom asm call we treat x8 as the 9th SMC argument
 *
 * We pass: fid/token/c0-c5 in x0-x7, c6/c7 on stack.
 * This function:
 *   1. Loads c6 from stack[0], c7 from stack[1]
 *   2. Places c6/c7 into x8/x9 for the RSI SMC ABI
 *   3. Issues SMC #0
 *   4. Returns x0
 */

.global rsi_attest_init_smc
.type   rsi_attest_init_smc, %function
rsi_attest_init_smc:
    /* x0=fid, x1=token_buf_ipa, x2=c0, ..., x7=c5 */
    /* [sp+0]=c6, [sp+8]=c7                         */
    stp  x8,  x9,  [sp, #-16]!  /* save callee-saved regs        */

    ldr  x8,  [sp, #16]          /* c6 (first stack arg)          */
    ldr  x9,  [sp, #24]          /* c7 (second stack arg)         */

    smc  #0

    ldp  x8,  x9,  [sp],  #16    /* restore                       */
    ret
.size rsi_attest_init_smc, . - rsi_attest_init_smc
