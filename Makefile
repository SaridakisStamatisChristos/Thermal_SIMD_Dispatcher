CC ?= gcc
ARCHFLAGS ?=
CFLAGS ?= -O2 -pthread -fPIC -mno-avx -Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wformat=2 -Werror=return-type
LDFLAGS ?= -pthread

BIN := thermal_simd
SRC := src/thermal_simd.c

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(ARCHFLAGS) -o $@ $< $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
