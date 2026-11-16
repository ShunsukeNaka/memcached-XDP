CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -fno-omit-frame-pointer
LDFLAGS ?= -pthread

mcload: mcload.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f mcload

.PHONY: clean
