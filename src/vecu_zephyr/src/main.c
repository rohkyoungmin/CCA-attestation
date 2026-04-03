#define _GNU_SOURCE
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include "rsi.h"

/* ------------------------------------------------------------------ */
/* Fixed challenge nonce (replace with real random in production)      */
/* ------------------------------------------------------------------ */
static const uint8_t g_challenge[RSI_CHALLENGE_SIZE] = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
	0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
	0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
	0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
	0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
};

/* Token buffer in shared memory (marked as SHARED IPA) */
static uint8_t __attribute__((section(".noinit")))
	token_buf[RSI_ATTEST_TOKEN_MAX_SIZE]
	__attribute__((aligned(RSI_GRANULE_SIZE)));

/* ------------------------------------------------------------------ */
/* Timing helpers                                                      */
/* ------------------------------------------------------------------ */
static inline uint64_t read_cntvct(void)
{
	uint64_t val;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r" (val));
	return val;
}

static inline uint64_t read_cntfrq(void)
{
	uint64_t val;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r" (val));
	return val;
}

static uint64_t cycles_to_ns(uint64_t cycles)
{
	uint64_t freq = read_cntfrq();
	if (freq == 0) return 0;
	return (cycles * 1000000000ULL) / freq;
}

/* ------------------------------------------------------------------ */
/* RSI init sequence                                                   */
/* ------------------------------------------------------------------ */
static int realm_init(void)
{
	uint64_t lower, upper;

	printk("\n=== ARM CCA Realm VM Init ===\n");

	/* 1. Check RSI ABI version */
	if (rsi_version_check(&lower, &upper) < 0) {
		printk("[REALM] RSI not available (not running in Realm?)\n");
		return -1;
	}

	/* 2. Mark token buffer as SHARED so Normal World can read it */
	uint64_t token_pa = (uint64_t)token_buf;
	if (rsi_ipa_state_set(token_pa,
			      token_pa + RSI_GRANULE_SIZE,
			      RSI_IPA_STATE_SHARED) < 0) {
		printk("[REALM] failed to mark token buffer as shared\n");
		return -1;
	}

	/* 3. Mark comm_ctrl as SHARED */
	comm_ctrl_t *ctrl = (comm_ctrl_t *)COMM_CTRL_BASE;
	uint64_t ctrl_pa = (uint64_t)ctrl;
	if (rsi_ipa_state_set(ctrl_pa,
			      ctrl_pa + RSI_GRANULE_SIZE,
			      RSI_IPA_STATE_SHARED) < 0) {
		printk("[REALM] failed to mark ctrl block as shared\n");
		return -1;
	}

	/* Initialize comm control block */
	memset(ctrl, 0, sizeof(*ctrl));
	memcpy(ctrl->challenge, g_challenge, RSI_CHALLENGE_SIZE);
	ctrl->magic = COMM_MAGIC;

	printk("[REALM] shared memory ready: token=0x%llx ctrl=0x%llx\n",
	       token_pa, ctrl_pa);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Attestation measurement loop                                        */
/* ------------------------------------------------------------------ */
#define ATTEST_ITERATIONS  10

static void run_attestation_benchmark(void)
{
	uint64_t token_size = 0;
	uint64_t freq = read_cntfrq();
	uint64_t total_cycles = 0;
	uint64_t min_cycles = UINT64_MAX;
	uint64_t max_cycles = 0;
	int success = 0;

	comm_ctrl_t *ctrl = (comm_ctrl_t *)COMM_CTRL_BASE;

	printk("\n=== Attestation Benchmark (%d iterations) ===\n",
	       ATTEST_ITERATIONS);
	printk("    token_buf PA: 0x%llx\n", (uint64_t)token_buf);
	printk("    cntfrq: %llu Hz\n", freq);

	for (int i = 0; i < ATTEST_ITERATIONS; i++) {
		memset(token_buf, 0, RSI_ATTEST_TOKEN_MAX_SIZE);
		token_size = 0;

		uint64_t t0 = read_cntvct();

		int ret = rsi_attestation_get_token(
				g_challenge,
				token_buf,
				RSI_ATTEST_TOKEN_MAX_SIZE,
				&token_size);

		uint64_t t1 = read_cntvct();
		uint64_t cycles = t1 - t0;
		uint64_t ns = cycles_to_ns(cycles);

		if (ret == 0) {
			success++;
			total_cycles += cycles;
			if (cycles < min_cycles) min_cycles = cycles;
			if (cycles > max_cycles) max_cycles = cycles;

			printk("  [%2d] token_size=%llu  time=%llu ns\n",
			       i, token_size, ns);

			/* Update shared comm block */
			ctrl->token_ready  = 1;
			ctrl->token_size   = (uint32_t)token_size;
			ctrl->gen_count    = (uint32_t)(i + 1);
			ctrl->gen_time_ns  = ns;
		} else {
			printk("  [%2d] FAILED\n", i);
		}

		k_sleep(K_MSEC(10));
	}

	if (success > 0) {
		uint64_t avg_ns = cycles_to_ns(total_cycles / success);
		printk("\n--- Results ---\n");
		printk("  Iterations : %d / %d succeeded\n",
		       success, ATTEST_ITERATIONS);
		printk("  Mean       : %llu ns\n", avg_ns);
		printk("  Min        : %llu ns\n", cycles_to_ns(min_cycles));
		printk("  Max        : %llu ns\n", cycles_to_ns(max_cycles));
		printk("  Token size : %llu bytes\n", token_size);
	}
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
	printk("\n");
	printk("╔══════════════════════════════════════════╗\n");
	printk("║   SCRUTINIZER V-ECU1 (Zephyr/Realm)     ║\n");
	printk("║   ARM CCA RSI Attestation Benchmark      ║\n");
	printk("╚══════════════════════════════════════════╝\n");

	if (realm_init() < 0) {
		printk("[MAIN] Realm init failed - running without RSI\n");
		printk("[MAIN] (expected if not under lkvm --realm)\n");
	} else {
		run_attestation_benchmark();
	}

	printk("\n[MAIN] Done. Spinning.\n");
	while (1) {
		k_sleep(K_SECONDS(5));
		printk("[MAIN] heartbeat\n");
	}

	return 0;
}
