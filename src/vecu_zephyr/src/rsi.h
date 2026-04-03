#ifndef RSI_H
#define RSI_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* RSI Function IDs (ARM CCA RMM Specification v1.0)                  */
/* ------------------------------------------------------------------ */
#define RSI_FID(n)                      (0xC4000000UL | (n))

#define RSI_VERSION                     RSI_FID(0x190)
#define RSI_FEATURES                    RSI_FID(0x191)
#define RSI_REALM_CONFIG                RSI_FID(0x192)
#define RSI_IPA_STATE_SET               RSI_FID(0x196)
#define RSI_IPA_STATE_GET               RSI_FID(0x197)
#define RSI_ATTESTATION_TOKEN_INIT      RSI_FID(0x198)
#define RSI_ATTESTATION_TOKEN_CONTINUE  RSI_FID(0x199)
#define RSI_HOST_CALL                   RSI_FID(0x19a)
#define RSI_MEASUREMENT_READ            RSI_FID(0x193)
#define RSI_MEASUREMENT_EXTEND          RSI_FID(0x194)

/* ------------------------------------------------------------------ */
/* RSI Return Codes                                                    */
/* ------------------------------------------------------------------ */
#define RSI_SUCCESS        0UL
#define RSI_ERROR_INPUT    1UL
#define RSI_ERROR_STATE    2UL
#define RSI_INCOMPLETE     3UL

/* ------------------------------------------------------------------ */
/* IPA State                                                           */
/* ------------------------------------------------------------------ */
#define RSI_IPA_STATE_PRIVATE  0UL
#define RSI_IPA_STATE_SHARED   1UL

/* ------------------------------------------------------------------ */
/* ABI Version                                                         */
/* ------------------------------------------------------------------ */
#define RSI_ABI_VERSION_MAJOR  1UL
#define RSI_ABI_VERSION_MINOR  0UL
#define RSI_ABI_VERSION        ((RSI_ABI_VERSION_MAJOR << 16) | RSI_ABI_VERSION_MINOR)

/* ------------------------------------------------------------------ */
/* Memory layout constants                                             */
/* Shared memory region: top 8KB of 64MB RAM                         */
/* ------------------------------------------------------------------ */
#define REALM_RAM_BASE       0x80000000UL
#define REALM_RAM_SIZE       (64UL * 1024 * 1024)
#define RSI_GRANULE_SIZE     4096UL

/* Shared memory: last 2 granules of RAM */
#define SHARED_MEM_BASE      (REALM_RAM_BASE + REALM_RAM_SIZE - 2 * RSI_GRANULE_SIZE)
#define ATTEST_TOKEN_BASE    SHARED_MEM_BASE                        /* 4KB: token */
#define COMM_CTRL_BASE       (SHARED_MEM_BASE + RSI_GRANULE_SIZE)   /* 4KB: control */

/* Max attestation token size (one granule) */
#define RSI_ATTEST_TOKEN_MAX_SIZE  RSI_GRANULE_SIZE

/* Challenge size (64 bytes = 512 bits) */
#define RSI_CHALLENGE_SIZE   64

/* ------------------------------------------------------------------ */
/* Communication control block (shared with Normal World)              */
/* ------------------------------------------------------------------ */
#define COMM_MAGIC           0xCCA0CC00UL

typedef struct {
	uint32_t magic;           /* COMM_MAGIC when valid */
	uint32_t token_ready;     /* 1 = attestation token written */
	uint32_t token_size;      /* actual token bytes */
	uint32_t gen_count;       /* number of tokens generated */
	uint64_t gen_time_ns;     /* last generation time (ns) */
	uint8_t  challenge[RSI_CHALLENGE_SIZE];
} comm_ctrl_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */
int  rsi_version_check(uint64_t *lower, uint64_t *upper);
int  rsi_ipa_state_set(uint64_t base, uint64_t top, uint64_t state);
int  rsi_shared_mem_init(void);
int  rsi_attestation_token_init(const uint8_t *challenge, uint64_t *out_size);
int  rsi_attestation_token_continue(uint64_t addr, uint64_t offset,
                                    uint64_t size, uint64_t *written);
int  rsi_attestation_get_token(const uint8_t *challenge,
                               uint8_t *token_buf, uint64_t buf_size,
                               uint64_t *token_size);

#endif /* RSI_H */
