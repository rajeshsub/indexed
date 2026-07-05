#!/usr/bin/env bash
set -euo pipefail

cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --parallel
ctest --test-dir build/debug --output-on-failure
