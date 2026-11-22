#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build
gcc -std=c11 -Wall -Wextra -Werror -g -O0 -Iinc -pthread \
    -DMAC_STORE_TESTING tests/mac_store_check.c src/mac_store.c \
    -lrt -o build/mac_store_check
