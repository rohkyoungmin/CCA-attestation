/*
 * attest_gen.c — CCA Attestation Token Generator (Software Simulation)
 *
 * Simulates ARM CCA RSI_ATTEST_TOKEN_INIT/CONTINUE using:
 *   - ECDSA-P384 signing  (same curve as ARM CCA platform token)
 *   - Minimal CBOR encoding of realm claims
 *
 * Measures: token generation time (CLOCK_MONOTONIC_RAW), absolute + printed
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/pem.h>

#define CHALLENGE_LEN  64
#define CLAIM_BUF_SIZE 512
#define TOKEN_BUF_SIZE 1024
#define ITERATIONS     200

/* common timer */
static inline int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

/*
 * Minimal CBOR encoder for a fixed-structure realm token map:
 *   { 10: challenge(64B), 44234: realm_hash(48B), 44241: pub_key(97B) }
 * Returns encoded length.
 */
static size_t encode_realm_claims_cbor(uint8_t *buf, size_t buflen,
                                       const uint8_t *challenge,
                                       const uint8_t *realm_hash,
                                       const uint8_t *pub_key) {
    size_t pos = 0;
#define PUT(b)   do { buf[pos++] = (b); } while(0)
#define PUT_BSTR(data, len) do {                                   \
    if ((len) <= 23) { PUT(0x40 | (len)); }                        \
    else if ((len) <= 0xFF) { PUT(0x58); PUT((uint8_t)(len)); }    \
    memcpy(buf + pos, data, len); pos += (len);                    \
} while(0)
#define PUT_UINT(n) do {                                           \
    if ((n) <= 23) { PUT(n); }                                     \
    else if ((n) <= 0xFF) { PUT(0x18); PUT((uint8_t)(n)); }        \
    else { PUT(0x19); PUT((uint8_t)((n)>>8)); PUT((uint8_t)(n)); } \
} while(0)

    (void)buflen;
    /* map(3) */
    PUT(0xa3);
    /* key 10 (nonce/challenge), value bstr(64) */
    PUT_UINT(10); PUT_BSTR(challenge, CHALLENGE_LEN);
    /* key 44234 (realm_hash), value bstr(48) */
    PUT_UINT(44234); PUT_BSTR(realm_hash, 48);
    /* key 44241 (pub_key), value bstr(97) */
    PUT_UINT(44241); PUT_BSTR(pub_key, 97);
#undef PUT
#undef PUT_BSTR
#undef PUT_UINT
    return pos;
}

int main(void) {
    /* Generate ECDSA-P384 key (done once, simulating RMM's realm key) */
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    EVP_PKEY_keygen_init(kctx);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_secp384r1);
    EVP_PKEY_keygen(kctx, &pkey);
    EVP_PKEY_CTX_free(kctx);

    uint8_t challenge[CHALLENGE_LEN];
    uint8_t realm_hash[48];
    uint8_t pub_key[97];
    uint8_t claim_buf[CLAIM_BUF_SIZE];
    uint8_t sig_buf[256];
    size_t sig_len;
    size_t claim_len = 0;

    /* Fixed realm_hash and pub_key (would come from RMM in real CCA) */
    memset(realm_hash, 0xAB, sizeof(realm_hash));
    memset(pub_key,    0xCD, sizeof(pub_key));

    int64_t timings[ITERATIONS];
    size_t  token_sizes[ITERATIONS];

    printf("=== ATTESTATION TOKEN GENERATION (CLOCK_MONOTONIC_RAW) ===\n");
    printf("  Algorithm : ECDSA-P384 (same as ARM CCA platform token)\n");
    printf("  Iterations: %d\n\n", ITERATIONS);

    for (int i = 0; i < ITERATIONS; i++) {
        /* Fresh challenge per iteration (nonce) */
        RAND_bytes(challenge, CHALLENGE_LEN);

        int64_t t0 = now_us();

        /* Step 1: Encode realm claims as CBOR (≈ RSI_ATTEST_TOKEN_INIT) */
        claim_len = encode_realm_claims_cbor(
            claim_buf, sizeof(claim_buf), challenge, realm_hash, pub_key);

        /* Step 2: ECDSA-P384 sign the claims (≈ RSI crypto inside RMM) */
        EVP_MD_CTX *mctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(mctx, NULL, EVP_sha384(), NULL, pkey);
        EVP_DigestSignUpdate(mctx, claim_buf, claim_len);
        sig_len = sizeof(sig_buf);
        EVP_DigestSignFinal(mctx, sig_buf, &sig_len);
        EVP_MD_CTX_free(mctx);

        int64_t elapsed = now_us() - t0;
        timings[i] = elapsed;
        token_sizes[i] = claim_len + sig_len;
    }

    /* Statistics */
    int64_t sum = 0, minv = timings[0], maxv = timings[0];
    int64_t sorted[ITERATIONS];
    memcpy(sorted, timings, sizeof(timings));
    for (int i = 0; i < ITERATIONS; i++) {
        sum += timings[i];
        if (timings[i] < minv) minv = timings[i];
        if (timings[i] > maxv) maxv = timings[i];
        sorted[i] = timings[i];
    }
    /* simple insertion sort for percentiles */
    for (int i = 1; i < ITERATIONS; i++) {
        int64_t key = sorted[i]; int j = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; j--; }
        sorted[j+1] = key;
    }

    printf("[ATTEST_GEN] N=%d  token_size=%zu bytes\n", ITERATIONS, token_sizes[0]);
    printf("  Mean  : %lld us\n",  (long long)(sum / ITERATIONS));
    printf("  Min   : %lld us\n",  (long long)minv);
    printf("  P50   : %lld us\n",  (long long)sorted[ITERATIONS/2]);
    printf("  P95   : %lld us\n",  (long long)sorted[(int)(ITERATIONS*0.95)]);
    printf("  P99   : %lld us\n",  (long long)sorted[(int)(ITERATIONS*0.99)]);
    printf("  Max   : %lld us\n",  (long long)maxv);

    /* Write last token to file for transfer test */
    FILE *f = fopen("/tmp/cca_token.bin", "wb");
    if (f) {
        uint32_t total_len = (uint32_t)(token_sizes[0]);
        fwrite(&total_len, 4, 1, f);
        fwrite(claim_buf, claim_len, 1, f);
        fwrite(sig_buf, sig_len, 1, f);
        fclose(f);
        /* Write pkey for verifier */
        FILE *kf = fopen("/tmp/cca_pkey.pem", "wb");
        if (kf) {
            PEM_write_PrivateKey(kf, pkey, NULL, NULL, 0, NULL, NULL);
            fclose(kf);
        }
        FILE *pkf = fopen("/tmp/cca_pubkey.pem", "wb");
        if (pkf) {
            PEM_write_PUBKEY(pkf, pkey);
            fclose(pkf);
        }
    }

    EVP_PKEY_free(pkey);
    return 0;
}
