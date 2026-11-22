#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build
cc=(gcc -std=c11 -Wall -Wextra -Werror -g -O0 -Iinc -pthread)
"${cc[@]}" bonus/supervisor/switch_supervisor.c src/mac_store.c -lrt -o build/switch_supervisor
"${cc[@]}" tests/supervisor_probe.c src/mac_store.c -lrt -o build/supervisor_probe
printf '%s\n' 'PASS: supervisor and recovery probe built'
