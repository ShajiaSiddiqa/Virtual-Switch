#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build
cc=(gcc -std=c11 -Wall -Wextra -Werror -g -O0 -Iinc -pthread)
modules=(src/mac_store.c src/pipe_link.c src/async_logger.c src/rate_limit.c
    src/signal_control.c src/control.c src/frame_queue.c src/shared_frame.c
    src/frame_io.c src/frame.c src/io.c src/mac_table.c src/shm_stats.c)
"${cc[@]}" src/switch.c "${modules[@]}" -lrt -o switch
"${cc[@]}" src/swctl.c src/shm_stats.c -lrt -o swctl
"${cc[@]}" bonus/two_switches/two_switches.c -lrt -o build/two_switches
"${cc[@]}" tests/pipe_link_check.c src/pipe_link.c src/frame.c -o build/pipe_link_check
"${cc[@]}" tests/stats_instance_check.c src/shm_stats.c -lrt -o build/stats_instance_check
"${cc[@]}" -ffunction-sections -fdata-sections tests/two_switch_pipe_check.c \
    "${modules[@]}" -Wl,--gc-sections -lrt -o build/two_switch_pipe_check
printf '%s\n' 'PASS: switch, swctl, launcher and three checks built'
