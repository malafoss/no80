VERSION=$(file < VERSION)

all: no80

no80.o: no80.c
	gcc -c -o $@ $^ -std=gnu18 -Wall -flto -fgnu-tm -O3 -fdata-sections -ffunction-sections -DVERSION=$(VERSION)

no80: no80.o
	gcc -o $@ $^ -flto -s -static -Wl,--gc-sections -Wl,--strip-all -Wl,-z,norelro -Wl,--build-id=none -Wl,-O1 -Wl,--start-group -L/usr/local/lib -lwolfssl -lm -litm -Wl,--end-group

clean:
	rm -f no80 no80.o
