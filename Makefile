CC ?= gcc
ARCHFLAGS ?=
CFLAGS ?= -O2 -pthread -fPIC -mno-avx -Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wformat=2 -Werror=return-type
LDFLAGS ?= -pthread

BIN := thermal_simd
SRC := src/thermal_simd.c src/config_parser.c src/statistics.c \
src/telemetry_helper.c src/thermal_config.c src/thermal_cpu.c src/thermal_trampoline.c \
src/thermal_perf.c src/thermal_signals.c

.PHONY: all clean run

all: $(BIN)

INCLUDES := -Isrc -Iinclude

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(ARCHFLAGS) $(INCLUDES) -o $@ $(SRC) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
