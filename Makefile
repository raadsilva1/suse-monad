CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=

all: suse-monad

suse-monad: suse-monad.c
	$(CC) $(CFLAGS) -o $@ suse-monad.c $(LDFLAGS)

clean:
	rm -f suse-monad
