#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build
cc=(gcc -std=c11 -Wall -Wextra -Werror -g -O0 -Iinc -pthread)
modules=(src/mac_store.c src/pipe_link.c src/async_logger.c src/rate_limit.c
    src/signal_control.c src/control.c src/frame_queue.c src/shared_frame.c
    src/frame_io.c src/frame.c src/io.c src/mac_table.c src/shm_stats.c)
mode=${1:-normal}
if [ "$mode" = tsan ]; then
    "${cc[@]}" -fsanitize=thread -DMAC_AGE_SECONDS=60 -DMAC_TABLE_PORT_LIMIT=1 \
        src/switch.c "${modules[@]}" -lrt -o build/switch_recovery_tsan
    "${cc[@]}" -fsanitize=thread -DMAC_TABLE_CAPACITY=3 -DMAC_TABLE_PORT_LIMIT=8 \
        tests/mac_recovery_check.c src/mac_table.c src/mac_store.c -lrt -o build/mac_recovery_check_tsan
    printf '%s\n' 'PASS: separate TSan recovery binaries built'
    exit 0
fi
if [ "$mode" != normal ]; then
    echo 'Usage: build_recovery.sh [normal|tsan]' >&2
    exit 1
fi
"${cc[@]}" src/switch.c "${modules[@]}" -lrt -o switch
# Longer age isolates crash/reconnect tests from ordinary five-second expiration.
# Strict one-MAC active limit also tests that recovered hints do not consume it.
"${cc[@]}" -DMAC_AGE_SECONDS=60 -DMAC_TABLE_PORT_LIMIT=1 \
    src/switch.c "${modules[@]}" -lrt -o build/switch_recovery
"${cc[@]}" src/swctl.c src/shm_stats.c -lrt -o swctl
"${cc[@]}" -DMAC_TABLE_CAPACITY=3 -DMAC_TABLE_PORT_LIMIT=8 \
    tests/mac_recovery_check.c src/mac_table.c src/mac_store.c -lrt -o build/mac_recovery_check
bash bonus/supervisor/build_supervisor.sh
printf '%s\n' 'PASS: ordinary switch, recovery-test switch, swctl and recovery unit check built'
