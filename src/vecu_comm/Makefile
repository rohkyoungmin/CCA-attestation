CC      = gcc
CFLAGS  = -O2 -Wall -std=c11 -D_GNU_SOURCE
LDFLAGS = -lssl -lcrypto -lpthread

all: vecu_comm

vecu_comm: vecu_comm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f vecu_comm /tmp/vecu_cert.pem /tmp/vecu_key.pem
