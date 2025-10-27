#!/usr/bin/env bash
set -euo pipefail
make clean && make
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
echo "OK: build"
