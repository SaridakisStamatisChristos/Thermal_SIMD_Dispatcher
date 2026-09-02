CXX ?= g++
CC ?= gcc
PKG_CONFIG ?= pkg-config
ARCHFLAGS ?=

OPENSSL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
OPENSSL_LIBS ?= $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null)
ifeq ($(strip $(OPENSSL_LIBS)),)
OPENSSL_LIBS := -lssl -lcrypto
endif

CPPFLAGS ?= -Isrc -Iinclude $(OPENSSL_CFLAGS) -DTSD_DEFAULT_COEFF_PATH='"$(CURDIR)/config/controller_coeffs.json"'
CFLAGS ?= -O2 -pthread -fPIC -mno-avx -Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wformat=2 -Werror=return-type
CXXFLAGS ?= -O2 -pthread -fPIC -mno-avx -std=c++17 -Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wformat=2
LDFLAGS ?= -pthread
LDLIBS ?= $(OPENSSL_LIBS)

BIN := thermal_simd

CORE_C := \
	src/config/runtime_flags.c \
	src/dispatch/adaptive_dispatch.c \
	src/runtime_guard.c \
	src/runtime_api.c \
	src/logging.c \
	src/config_parser.c \
	src/third_party/jsmn.c \
	src/statistics.c \
	src/runtime_metrics.c \
	src/health_check.c \
	src/telemetry_helper.c \
	src/thermal_config.c \
	src/thermal_cpu.c \
	src/patcher/trampoline_guard.c \
	src/thermal_perf.c \
	src/thermal_signals.c \
	src/policy/policy_config.c

CORE_CPP := \
	src/healthcheck/sandbox.cpp \
	src/telemetry/evaluator.cpp \
	src/telemetry/history_store.cpp \
	src/telemetry/sensors.cpp \
	src/telemetry/bus.cpp \
	src/telemetry/collector.cpp \
	src/telemetry/fusion.cpp \
	src/telemetry/fusion_bridge.cpp \
	src/observability/metrics.cpp \
	src/observability/statsd_exporter.cpp \
	src/observability/telemetry_state.cpp \
	src/patcher/trampoline.cpp \
	src/policy/dispatcher_policy.cpp \
	src/policy/arx_model.cpp \
	src/policy/mpc_controller.cpp

APP_C := src/thermal_simd.c
APP_CPP := src/main.cpp

SRC_C := $(CORE_C) $(APP_C)
SRC_CPP := $(CORE_CPP) $(APP_CPP)
OBJ_C := $(SRC_C:.c=.o)
OBJ_CPP := $(SRC_CPP:.cpp=.o)
OBJ := $(OBJ_C) $(OBJ_CPP)

.PHONY: all clean run check-deps

all: $(BIN)

check-deps:
	@printf 'OpenSSL libs: %s\n' "$(OPENSSL_LIBS)"

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/thermal_simd.o: CPPFLAGS += -DTSD_RUNTIME_INTERNAL_IMPL
src/patcher/trampoline.o: CPPFLAGS += -DTSD_TRAMPOLINE_INTERNAL_IMPL

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCHFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ARCHFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) $(OBJ)
