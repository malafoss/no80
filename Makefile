VERSION=$(file < VERSION)
CC=gcc
CFLAGS=-std=gnu18 -Wall -flto -O3 -fdata-sections -ffunction-sections -DVERSION=$(VERSION)
LDFLAGS=-flto -s -static -Wl,--gc-sections -Wl,--strip-all -Wl,-z,norelro -Wl,--build-id=none -Wl,-O1

all: no80

no80.o: no80.c

no80: no80.o

clean:
	rm -f no80 no80.o
