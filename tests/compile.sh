#!/usr/bin/env bash
set -euo pipefail
make clean && make
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
echo "OK: build"
