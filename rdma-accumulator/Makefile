CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -libverbs -lpthread

all: server client

server: server_array.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

client: client_independent_buffer.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f server client

.PHONY: all clean