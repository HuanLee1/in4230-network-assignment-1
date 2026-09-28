CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic

COMMON = common.c

all: mipd ping_client ping_server

mipd: mipd.c $(COMMON) common.h
	$(CC) $(CFLAGS) mipd.c $(COMMON) -o mipd

ping_client: ping_client.c $(COMMON) common.h
	$(CC) $(CFLAGS) ping_client.c $(COMMON) -o ping_client

ping_server: ping_server.c $(COMMON) common.h
	$(CC) $(CFLAGS) ping_server.c $(COMMON) -o ping_server

clean:
	rm -f mipd ping_client ping_server