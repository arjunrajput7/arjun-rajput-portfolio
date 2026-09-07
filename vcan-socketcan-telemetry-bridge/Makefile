CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  := -lm

BIN := can_tx can_rx

all: $(BIN)

can_tx: src/can_tx.c src/can_common.h
	$(CC) $(CFLAGS) -o $@ src/can_tx.c $(LDLIBS)

can_rx: src/can_rx.c src/can_common.h
	$(CC) $(CFLAGS) -o $@ src/can_rx.c $(LDLIBS)

clean:
	rm -f $(BIN)

.PHONY: all clean
