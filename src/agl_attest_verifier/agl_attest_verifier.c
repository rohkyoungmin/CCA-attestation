/*
 * AGL-side CCA token verifier/consumer for the paper-facing Realm demo.
 *
 * The Realm kvmtool process listens on the FVP host. This process runs inside
 * the AGL guest and connects to the kvmtool user-network host address
 * 192.168.33.1. For each request it receives a real RSI attestation token,
 * performs deterministic AGL-side token processing, and returns timing data.
 *
 * Current verification scope:
 * - size bounds
 * - definite-length CBOR structural validation
 * - SHA-256 digest of the complete token
 *
 * Full CCA trust-chain verification can be layered on top when the platform
 * attestation public key/certificate material is available in the AGL image.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define AGL_PROTO_MAGIC        0x314c4741U
#define AGL_PROTO_VERSION      1U
#define AGL_DEFAULT_HOST       "192.168.33.1"
#define AGL_DEFAULT_PORT       7777
#define AGL_MAX_TOKEN_SIZE     4096U
#define AGL_CONNECT_TIMEOUT_MS 1000

struct agl_verify_req {
	uint32_t magic;
	uint32_t version;
	uint32_t gen;
	uint32_t token_size;
	uint64_t realm_gen_time_ns;
};

struct agl_verify_resp {
	uint32_t magic;
	uint32_t version;
	uint32_t gen;
	uint32_t status;
	uint64_t parse_ns;
	uint64_t hash_ns;
	uint64_t total_ns;
	uint8_t sha256[32];
};

struct sha256_ctx {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t data[64];
	size_t datalen;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint32_t rotr32(uint32_t x, uint32_t n)
{
	return (x >> n) | (x << (32U - n));
}

static void sha256_transform(struct sha256_ctx *ctx, const uint8_t data[64])
{
	static const uint32_t k[64] = {
		0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
		0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
		0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
		0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
		0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
		0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
		0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
		0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
		0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
		0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
		0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
		0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
		0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
		0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
		0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
		0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
	};
	uint32_t m[64];
	uint32_t a, b, c, d, e, f, g, h;

	for (size_t i = 0; i < 16; i++) {
		m[i] = ((uint32_t)data[i * 4] << 24) |
		       ((uint32_t)data[i * 4 + 1] << 16) |
		       ((uint32_t)data[i * 4 + 2] << 8) |
		       ((uint32_t)data[i * 4 + 3]);
	}
	for (size_t i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^ (m[i - 15] >> 3);
		uint32_t s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^ (m[i - 2] >> 10);
		m[i] = m[i - 16] + s0 + m[i - 7] + s1;
	}

	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

	for (size_t i = 0; i < 64; i++) {
		uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + s1 + ch + k[i] + m[i];
		uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = s0 + maj;

		h = g; g = f; f = e; e = d + temp1;
		d = c; c = b; b = a; a = temp1 + temp2;
	}

	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(struct sha256_ctx *ctx)
{
	ctx->datalen = 0;
	ctx->bitlen = 0;
	ctx->state[0] = 0x6a09e667U; ctx->state[1] = 0xbb67ae85U;
	ctx->state[2] = 0x3c6ef372U; ctx->state[3] = 0xa54ff53aU;
	ctx->state[4] = 0x510e527fU; ctx->state[5] = 0x9b05688cU;
	ctx->state[6] = 0x1f83d9abU; ctx->state[7] = 0x5be0cd19U;
}

static void sha256_update(struct sha256_ctx *ctx, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		ctx->data[ctx->datalen++] = data[i];
		if (ctx->datalen == 64) {
			sha256_transform(ctx, ctx->data);
			ctx->bitlen += 512;
			ctx->datalen = 0;
		}
	}
}

static void sha256_final(struct sha256_ctx *ctx, uint8_t hash[32])
{
	size_t i = ctx->datalen;

	ctx->data[i++] = 0x80;
	if (i > 56) {
		while (i < 64)
			ctx->data[i++] = 0;
		sha256_transform(ctx, ctx->data);
		i = 0;
	}
	while (i < 56)
		ctx->data[i++] = 0;

	ctx->bitlen += ctx->datalen * 8ULL;
	for (int j = 7; j >= 0; j--)
		ctx->data[63 - j] = (uint8_t)(ctx->bitlen >> (j * 8));
	sha256_transform(ctx, ctx->data);

	for (i = 0; i < 4; i++) {
		for (size_t j = 0; j < 8; j++)
			hash[j * 4 + i] = (uint8_t)(ctx->state[j] >> (24 - i * 8));
	}
}

static int cbor_read_arg(const uint8_t *buf, size_t len, size_t *off,
			 uint8_t ai, uint64_t *value)
{
	if (ai < 24) {
		*value = ai;
		return 0;
	}
	if (ai == 24) {
		if (*off + 1 > len)
			return -1;
		*value = buf[(*off)++];
		return 0;
	}
	if (ai == 25) {
		if (*off + 2 > len)
			return -1;
		*value = ((uint64_t)buf[*off] << 8) | buf[*off + 1];
		*off += 2;
		return 0;
	}
	if (ai == 26) {
		if (*off + 4 > len)
			return -1;
		*value = ((uint64_t)buf[*off] << 24) | ((uint64_t)buf[*off + 1] << 16) |
			 ((uint64_t)buf[*off + 2] << 8) | buf[*off + 3];
		*off += 4;
		return 0;
	}
	if (ai == 27) {
		if (*off + 8 > len)
			return -1;
		*value = 0;
		for (int i = 0; i < 8; i++)
			*value = (*value << 8) | buf[(*off)++];
		return 0;
	}

	return -1;
}

static int cbor_skip_one(const uint8_t *buf, size_t len, size_t *off, int depth)
{
	uint8_t hdr, major, ai;
	uint64_t n;

	if (depth > 32 || *off >= len)
		return -1;

	hdr = buf[(*off)++];
	major = hdr >> 5;
	ai = hdr & 0x1f;

	if (major == 7) {
		if (ai < 24)
			return 0;
		if (ai == 24)
			return (*off + 1 <= len) ? ((*off += 1), 0) : -1;
		if (ai == 25)
			return (*off + 2 <= len) ? ((*off += 2), 0) : -1;
		if (ai == 26)
			return (*off + 4 <= len) ? ((*off += 4), 0) : -1;
		if (ai == 27)
			return (*off + 8 <= len) ? ((*off += 8), 0) : -1;
		return -1;
	}

	if (cbor_read_arg(buf, len, off, ai, &n) != 0)
		return -1;

	if (major == 0 || major == 1)
		return 0;
	if (major == 2 || major == 3) {
		if (n > len || *off + (size_t)n > len)
			return -1;
		*off += (size_t)n;
		return 0;
	}
	if (major == 4) {
		for (uint64_t i = 0; i < n; i++) {
			if (cbor_skip_one(buf, len, off, depth + 1) != 0)
				return -1;
		}
		return 0;
	}
	if (major == 5) {
		for (uint64_t i = 0; i < n; i++) {
			if (cbor_skip_one(buf, len, off, depth + 1) != 0 ||
			    cbor_skip_one(buf, len, off, depth + 1) != 0)
				return -1;
		}
		return 0;
	}
	if (major == 6)
		return cbor_skip_one(buf, len, off, depth + 1);

	return -1;
}

static int cbor_validate(const uint8_t *buf, size_t len)
{
	size_t off = 0;

	if (cbor_skip_one(buf, len, &off, 0) != 0)
		return -1;

	return off == len ? 0 : -1;
}

static int read_full(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static int connect_once(const char *host, int port)
{
	struct sockaddr_in addr;
	struct pollfd pfd;
	int fd;
	int flags;
	int err = 0;
	socklen_t err_len = sizeof(err);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		fprintf(stderr, "invalid host: %s\n", host);
		exit(2);
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		goto connected;

	if (errno != EINPROGRESS) {
		close(fd);
		return -1;
	}

	pfd.fd = fd;
	pfd.events = POLLOUT;
	if (poll(&pfd, 1, AGL_CONNECT_TIMEOUT_MS) <= 0) {
		close(fd);
		return -1;
	}

	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 ||
	    err != 0) {
		close(fd);
		errno = err;
		return -1;
	}

connected:
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags);
	return fd;
}

static int connect_loop(const char *host, int port)
{
	for (;;) {
		int fd = connect_once(host, port);

		if (fd >= 0)
			return fd;

		printf("[agl-verifier] connect retry host=%s port=%d errno=%d\n",
		       host, port, errno);
		fflush(stdout);
		sleep(1);
	}
}

int main(int argc, char **argv)
{
	const char *host = AGL_DEFAULT_HOST;
	int port = AGL_DEFAULT_PORT;
	uint8_t token[AGL_MAX_TOKEN_SIZE];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--host") && i + 1 < argc) {
			host = argv[++i];
		} else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			port = atoi(argv[++i]);
		} else {
			fprintf(stderr, "Usage: %s [--host ip] [--port port]\n", argv[0]);
			return 2;
		}
	}

	printf("[agl-verifier] connecting to %s:%d\n", host, port);
	fflush(stdout);

	for (;;) {
		int fd = connect_loop(host, port);

		printf("[agl-verifier] connected\n");
		fflush(stdout);

		for (;;) {
			struct agl_verify_req req;
			struct agl_verify_resp resp;
			struct sha256_ctx sha;
			uint64_t t0, t1, t2, t3;

			if (read_full(fd, &req, sizeof(req)) != 0)
				break;

			memset(&resp, 0, sizeof(resp));
			resp.magic = AGL_PROTO_MAGIC;
			resp.version = AGL_PROTO_VERSION;
			resp.gen = req.gen;
			resp.status = 1;

			if (req.magic != AGL_PROTO_MAGIC ||
			    req.version != AGL_PROTO_VERSION ||
			    req.token_size == 0 ||
			    req.token_size > sizeof(token) ||
			    read_full(fd, token, req.token_size) != 0) {
				(void)write_full(fd, &resp, sizeof(resp));
				break;
			}

			t0 = now_ns();
			resp.status = cbor_validate(token, req.token_size) == 0 ? 0 : 2;
			t1 = now_ns();

			sha256_init(&sha);
			sha256_update(&sha, token, req.token_size);
			sha256_final(&sha, resp.sha256);
			t2 = now_ns();
			t3 = now_ns();

			resp.parse_ns = t1 - t0;
			resp.hash_ns = t2 - t1;
			resp.total_ns = t3 - t0;

			printf("agl_csv,gen=%u,token_size=%u,realm_gen_ns=%llu,parse_ns=%llu,hash_ns=%llu,total_ns=%llu,status=0x%08x,sha256=%02x%02x%02x%02x...\n",
			       req.gen, req.token_size,
			       (unsigned long long)req.realm_gen_time_ns,
			       (unsigned long long)resp.parse_ns,
			       (unsigned long long)resp.hash_ns,
			       (unsigned long long)resp.total_ns,
			       resp.status,
			       resp.sha256[0], resp.sha256[1],
			       resp.sha256[2], resp.sha256[3]);
			fflush(stdout);

			if (write_full(fd, &resp, sizeof(resp)) != 0)
				break;
		}

		close(fd);
		printf("[agl-verifier] disconnected, retrying\n");
		fflush(stdout);
		sleep(1);
	}
}
