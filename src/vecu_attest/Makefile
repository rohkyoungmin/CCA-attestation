CC      = gcc
CFLAGS  = -O2 -Wall -std=c11 -D_GNU_SOURCE
LDFLAGS = -lssl -lcrypto

all: attest_gen token_verify

attest_gen: attest_gen.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

token_verify: token_verify.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f attest_gen token_verify /tmp/cca_token.bin /tmp/cca_pubkey.pem /tmp/cca_pkey.pem
