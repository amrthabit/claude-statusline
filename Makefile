# musl's process init is ~10us vs glibc's ~430us - a measurable win on a
# binary spawned every second. Byte-for-byte parity verified (make test).
# Falls back to gcc when musl-tools isn't installed.
CC      := $(shell command -v musl-gcc >/dev/null 2>&1 && echo musl-gcc || echo gcc)
CFLAGS  = -O2 -Wall -Wextra -std=c11
# Static: skips the dynamic linker at every render (~0.5ms).
# Safe here - no NSS/DNS/locale use. `make LDFLAGS=` for a dynamic build.
LDFLAGS = -static
LDLIBS  = -lm

statusline-bin: statusline.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ statusline.c $(LDLIBS)

test: statusline-bin
	bash test/parity.sh

clean:
	rm -f statusline-bin

.PHONY: test clean
