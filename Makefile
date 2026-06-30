CC     = gcc
MPICC  = mpicc
CFLAGS = -Wall -Wextra -g -O2

all: pulso_server pulso_client

pulso_server: pulso_server.c structs.h
	$(MPICC) $(CFLAGS) -o pulso_server pulso_server.c

pulso_client: pulso_client.c structs.h
	$(CC) $(CFLAGS) -o pulso_client pulso_client.c

clean:
	rm -f pulso_server pulso_client

.PHONY: all clean
