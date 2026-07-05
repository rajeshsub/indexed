#!/usr/bin/env bash
set -euo pipefail

CONFIG="${1:-release}"

case "$CONFIG" in
    release)
        cmake --preset linux-gcc-release
        cmake --build --preset linux-gcc-release --parallel
        ;;
    debug)
        cmake --preset linux-gcc-debug
        cmake --build --preset linux-gcc-debug --parallel
        ;;
    asan)
        cmake --preset linux-asan
        cmake --build --preset linux-asan --parallel
        ;;
    test)
        cmake --preset linux-gcc-debug
        cmake --build --preset linux-gcc-debug --parallel
        ctest --test-dir build/debug --output-on-failure
        ;;
    clean)
        rm -rf build/debug build/release build/asan build/clang-release
        ;;
    all)
        cmake --preset linux-gcc-debug   && cmake --build --preset linux-gcc-debug   --parallel
        cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release --parallel
        cmake --preset linux-asan        && cmake --build --preset linux-asan        --parallel
        ;;
    *)
        echo "Usage: build.sh [release|debug|asan|test|all|clean]"
        exit 1
        ;;
esac
