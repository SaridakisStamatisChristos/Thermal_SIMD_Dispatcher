CC ?= gcc
CFLAGS ?= -O2 -march=native -pthread -fPIC -mno-avx -Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wformat=2 -Werror=return-type
LDFLAGS ?= -pthread

BIN := thermal_simd
SRC := src/thermal_simd.c

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
