CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2 -g -pthread -D_DEFAULT_SOURCE
LDFLAGS = -pthread

all: server client

server: server.o common.o
	$(CC) $(CFLAGS) -o $@ server.o common.o $(LDFLAGS)

client: client.o common.o
	$(CC) $(CFLAGS) -o $@ client.o common.o $(LDFLAGS)

server.o: server.c common.h server.h
	$(CC) $(CFLAGS) -c server.c

client.o: client.c common.h
	$(CC) $(CFLAGS) -c client.c

common.o: common.c common.h
	$(CC) $(CFLAGS) -c common.c

clean:
	rm -f *.o server client server*.log client*.log

