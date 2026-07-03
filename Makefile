CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
# Static: skips the dynamic linker at every render (~0.5ms of ~1.8ms).
# Safe here - no NSS/DNS/locale use. `make LDFLAGS=` for a dynamic build.
LDFLAGS = -static
LDLIBS  = -lm

statusline-bin: statusline.c vendor/cJSON.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ statusline.c vendor/cJSON.o $(LDLIBS)

# vendored code built without -Wextra (not ours to lint)
vendor/cJSON.o: vendor/cJSON.c vendor/cJSON.h
	$(CC) -O2 -std=c11 -c -o $@ vendor/cJSON.c

test: statusline-bin
	bash test/parity.sh

clean:
	rm -f statusline-bin vendor/cJSON.o

.PHONY: test clean
