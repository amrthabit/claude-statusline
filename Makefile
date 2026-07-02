CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDLIBS  = -lm

statusline-bin: statusline.c vendor/cJSON.o
	$(CC) $(CFLAGS) -o $@ statusline.c vendor/cJSON.o $(LDLIBS)

# vendored code built without -Wextra (not ours to lint)
vendor/cJSON.o: vendor/cJSON.c vendor/cJSON.h
	$(CC) -O2 -std=c11 -c -o $@ vendor/cJSON.c

test: statusline-bin
	bash test/parity.sh

clean:
	rm -f statusline-bin vendor/cJSON.o

.PHONY: test clean
