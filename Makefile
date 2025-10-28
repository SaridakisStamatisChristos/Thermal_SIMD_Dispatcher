CXX ?= g++
CC ?= gcc
ARCHFLAGS ?=
CFLAGS ?= -O2 -pthread -fPIC -mno-avx -Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wformat=2 -Werror=return-type
CXXFLAGS ?= -O2 -pthread -fPIC -mno-avx -std=c++17 -Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wformat=2
LDFLAGS ?= -pthread

BIN := thermal_simd
SRC_C := src/thermal_simd.c src/logging.c src/config_parser.c src/statistics.c \
src/runtime_metrics.c src/health_check.c src/telemetry_helper.c src/thermal_config.c \
src/thermal_cpu.c src/thermal_perf.c src/thermal_signals.c \
src/policy/policy_config.c
SRC_CPP := src/patcher/trampoline.cpp src/telemetry/evaluator.cpp src/telemetry/history_store.cpp src/telemetry/sensors.cpp \
src/policy/dispatcher_policy.cpp src/policy/mpc_controller.cpp src/observability/metrics.cpp

OBJ_C := $(SRC_C:.c=.o)
OBJ_CPP := $(SRC_CPP:.cpp=.o)
OBJ := $(OBJ_C) $(OBJ_CPP)

.PHONY: all clean run

all: $(BIN)

INCLUDES := -Isrc -Iinclude

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(INCLUDES) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(ARCHFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(INCLUDES) -c $< -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) $(OBJ)
