#define _GNU_SOURCE
#include "rsi.h"
#include <zephyr/kernel.h>
#include <zephyr/arch/arm64/arm-smccc.h>
#include <zephyr/sys/printk.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal: make a generic RSI call via SMC                           */
/* ------------------------------------------------------------------ */
static inline uint64_t rsi_smc(uint64_t fid,
				uint64_t a1, uint64_t a2, uint64_t a3,
				uint64_t a4, uint64_t a5, uint64_t a6,
				uint64_t a7, uint64_t *res1)
{
	struct arm_smccc_res res;
	arm_smccc_smc(fid, a1, a2, a3, a4, a5, a6, a7, &res);
	if (res1) {
		*res1 = res.a1;
	}
	return res.a0;
}

/* ------------------------------------------------------------------ */
/* RSI_ATTESTATION_TOKEN_INIT: needs x0-x8 (FID + 8 challenge words)  */
/* Implemented in rsi_asm.S as rsi_attest_init_smc()                  */
/* ------------------------------------------------------------------ */
extern uint64_t rsi_attest_init_smc(uint64_t fid,
				    uint64_t c0, uint64_t c1, uint64_t c2,
				    uint64_t c3, uint64_t c4, uint64_t c5,
				    uint64_t c6, uint64_t c7,
				    uint64_t *out_size);

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int rsi_version_check(uint64_t *lower, uint64_t *upper)
{
	struct arm_smccc_res res;
	arm_smccc_smc(RSI_VERSION, RSI_ABI_VERSION, 0, 0, 0, 0, 0, 0, &res);
	if (lower) *lower = res.a1;
	if (upper) *upper = res.a2;
	if (res.a0 != RSI_SUCCESS) {
		printk("[RSI] version check failed: 0x%llx\n", res.a0);
		return -1;
	}
	printk("[RSI] ABI version OK: lower=0x%llx upper=0x%llx\n", res.a1, res.a2);
	return 0;
}

int rsi_ipa_state_set(uint64_t base, uint64_t top, uint64_t state)
{
	uint64_t out_top;
	uint64_t ret = rsi_smc(RSI_IPA_STATE_SET, base, top, state,
			       0, 0, 0, 0, &out_top);
	if (ret != RSI_SUCCESS) {
		printk("[RSI] IPA_STATE_SET failed: base=0x%llx ret=0x%llx\n",
		       base, ret);
		return -1;
	}
	return 0;
}

int rsi_shared_mem_init(void)
{
	uint64_t base = SHARED_MEM_BASE;
	uint64_t top  = SHARED_MEM_BASE + 2 * RSI_GRANULE_SIZE;

	printk("[RSI] marking shared mem: 0x%llx - 0x%llx\n", base, top);
	return rsi_ipa_state_set(base, top, RSI_IPA_STATE_SHARED);
}

int rsi_attestation_token_init(const uint8_t *challenge, uint64_t *out_size)
{
	uint64_t c[8] = {0};

	/* pack 64-byte challenge into 8 little-endian uint64_t */
	for (int i = 0; i < 8; i++) {
		for (int j = 0; j < 8; j++) {
			int idx = i * 8 + j;
			if (idx < RSI_CHALLENGE_SIZE) {
				c[i] |= ((uint64_t)challenge[idx]) << (j * 8);
			}
		}
	}

	uint64_t ret = rsi_attest_init_smc(RSI_ATTESTATION_TOKEN_INIT,
					   c[0], c[1], c[2], c[3],
					   c[4], c[5], c[6], c[7], out_size);
	if (ret != RSI_SUCCESS) {
		printk("[RSI] ATTEST_INIT failed: 0x%llx\n", ret);
		return -1;
	}
	return 0;
}

int rsi_attestation_token_continue(uint64_t addr, uint64_t offset,
				   uint64_t size, uint64_t *written)
{
	uint64_t w;
	uint64_t ret = rsi_smc(RSI_ATTESTATION_TOKEN_CONTINUE,
			       addr, offset, size, 0, 0, 0, 0, &w);
	if (written) {
		*written = w;
	}
	/* returns RSI_SUCCESS (done) or RSI_INCOMPLETE (more data) */
	return (int)ret;
}

int rsi_attestation_get_token(const uint8_t *challenge,
			      uint8_t *token_buf, uint64_t buf_size,
			      uint64_t *token_size)
{
	uint64_t upper_bound = 0;
	uint64_t offset = 0;

	if (rsi_attestation_token_init(challenge, &upper_bound) < 0) {
		return -1;
	}

	/* collect token granule by granule */
	while (1) {
		uint64_t written = 0;
		int ret = rsi_attestation_token_continue(
			(uint64_t)token_buf, offset,
			RSI_GRANULE_SIZE, &written);

		offset += written;

		if (ret == RSI_SUCCESS) {
			break;
		} else if (ret == RSI_INCOMPLETE) {
			if (offset >= buf_size) {
				printk("[RSI] token buffer overflow\n");
				return -1;
			}
			continue;
		} else {
			printk("[RSI] ATTEST_CONTINUE error: %d\n", ret);
			return -1;
		}
	}

	if (token_size) {
		*token_size = offset;
	}
	return 0;
}
