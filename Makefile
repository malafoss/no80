CC=gcc
CFLAGS=-O3 -flto -fdata-sections -ffunction-sections -Wall
LDFLAGS=-s -static -Wl,--gc-sections -Wl,--strip-all

all: no80

no80.o: no80.c

no80: no80.o

clean:
	rm -f no80 no80.o
