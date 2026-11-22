CC = gcc
CPPFLAGS = -Iinc -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread
LDLIBS = -pthread -lrt
PYTHON = python3
HEADERS := $(wildcard inc/*.h)
MODULES := $(filter-out src/switch.c src/host.c src/swctl.c,$(wildcard src/*.c))
PROGRAMS := switch host swctl build/two_switches build/switch_supervisor build/lock_bench
TEST_SOURCES := $(wildcard tests/*.c)
TEST_BINS := $(patsubst tests/%.c,build/%,$(TEST_SOURCES))
UNIT_NAMES := main mac_aging_count_check qos_check rate_limit_check logger_check mac_admin_check mac_vlan_check \
 mac_log_check mac_store_check mac_recovery_check pipe_link_check shm_stats_check \
 stats_instance_check vlan_forward_check mirror_forward_check port_admin_check two_switch_pipe_check
UNIT_BINS := $(addprefix build/,$(UNIT_NAMES))

.DEFAULT_GOAL := all
.PHONY: all clean tests unit-check check integration-check tsan tsan-check valgrind-check benchmark help audit report
all: $(PROGRAMS) $(TEST_BINS) build/switch_recovery
build:
	mkdir -p build
switch: src/switch.c $(MODULES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/switch.c $(MODULES) $(LDLIBS) -o $@
host: src/host.c src/frame_io.c src/frame.c src/io.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/host.c src/frame_io.c src/frame.c src/io.c $(LDLIBS) -o $@
swctl: src/swctl.c src/shm_stats.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/swctl.c src/shm_stats.c $(LDLIBS) -o $@
build/two_switches: bonus/two_switches/two_switches.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDLIBS) -o $@
build/switch_supervisor: bonus/supervisor/switch_supervisor.c src/mac_store.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/mac_store.c $(LDLIBS) -o $@
build/lock_bench: bonus/benchmark/lock_bench.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDLIBS) -o $@
build/lock_bench_release: bonus/benchmark/lock_bench.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O2 $< $(LDLIBS) -o $@

# Generic test rule also builds manual signal/statistics demos without running them.
build/%: tests/%.c $(MODULES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $< $(MODULES) -Wl,--gc-sections $(LDLIBS) -o $@
build/mac_store_check: tests/mac_store_check.c src/mac_store.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMAC_STORE_TESTING $< src/mac_store.c $(LDLIBS) -o $@
build/mac_recovery_check: tests/mac_recovery_check.c src/mac_table.c src/mac_store.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMAC_TABLE_CAPACITY=3 -DMAC_TABLE_PORT_LIMIT=8 $< src/mac_table.c src/mac_store.c $(LDLIBS) -o $@
build/switch_recovery: src/switch.c $(MODULES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMAC_AGE_SECONDS=60 -DMAC_TABLE_PORT_LIMIT=1 $< $(MODULES) $(LDLIBS) -o $@

tests: $(TEST_BINS) build/switch_recovery
unit-check: $(UNIT_BINS)
	@set -e; for test in $(UNIT_BINS); do echo "== $$test =="; "$$test"; done
integration-check: all tests
	bash tests/basic_forward.sh
	$(PYTHON) tests/host_check.py
	$(PYTHON) tests/signal_check.py
	$(PYTHON) tests/tier1_verify.py ./switch
	$(PYTHON) tests/vlan_integration.py
	$(PYTHON) tests/mirror_integration.py
	$(PYTHON) tests/port_admin_integration.py
	$(PYTHON) tests/logger_integration.py
	$(PYTHON) tests/two_switch_integration.py
	$(PYTHON) tests/supervisor_check.py
	bash tests/supervisor_recovery.sh
check: unit-check integration-check

build/switch_tsan: src/switch.c $(MODULES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=thread $< $(MODULES) $(LDLIBS) -o $@
build/host_tsan: src/host.c src/frame_io.c src/frame.c src/io.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=thread $< src/frame_io.c src/frame.c src/io.c $(LDLIBS) -o $@
build/switch_recovery_tsan: src/switch.c $(MODULES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=thread -DMAC_AGE_SECONDS=60 -DMAC_TABLE_PORT_LIMIT=1 $< $(MODULES) $(LDLIBS) -o $@
tsan: build/switch_tsan build/host_tsan build/switch_recovery_tsan
# Run with setarch -R externally only if this machine needs the TSan mapping workaround.
tsan-check: all tsan
	TSAN_OPTIONS=halt_on_error=1 SWITCH="$(CURDIR)/build/switch_tsan" bash tests/basic_forward.sh
	TSAN_OPTIONS=halt_on_error=1 HOST="$(CURDIR)/build/host_tsan" $(PYTHON) tests/host_check.py
	TSAN_OPTIONS=halt_on_error=1 $(PYTHON) tests/tier1_verify.py ./build/switch_tsan
	TSAN_OPTIONS=halt_on_error=1 SWITCH_COMMAND="$(CURDIR)/build/switch_tsan" $(PYTHON) tests/vlan_integration.py
	TSAN_OPTIONS=halt_on_error=1 SWITCH_COMMAND="$(CURDIR)/build/switch_tsan" $(PYTHON) tests/mirror_integration.py
	TSAN_OPTIONS=halt_on_error=1 SWITCH_COMMAND="$(CURDIR)/build/switch_tsan" $(PYTHON) tests/logger_integration.py
	TSAN_OPTIONS=halt_on_error=1 SWITCH_COMMAND="$(CURDIR)/build/switch_tsan" $(PYTHON) tests/port_admin_integration.py
	TSAN_OPTIONS=halt_on_error=1 VSWITCH_BINARY="$(CURDIR)/build/switch_tsan" $(PYTHON) tests/two_switch_integration.py
	TSAN_OPTIONS=halt_on_error=1 SWITCH="$(CURDIR)/build/switch_recovery_tsan" bash tests/supervisor_recovery.sh
valgrind-check: all $(UNIT_BINS)
	@set -e; for test in $(UNIT_BINS); do echo "== Memcheck $$test =="; valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=99 "$$test"; done
	$(PYTHON) tests/tier1_verify.py valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=99 ./switch
	HOST_COMMAND="valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=99 ./host" $(PYTHON) tests/host_check.py
benchmark: build/lock_bench_release
	BENCH_COMPILE_COMMAND="$(CC) $(CPPFLAGS) $(CFLAGS) -O2 bonus/benchmark/lock_bench.c $(LDLIBS) -o build/lock_bench_release" $(PYTHON) bonus/benchmark/run_benchmark.py --binary ./build/lock_bench_release --ops 5000000 --repeats 3
clean:
	rm -rf -- build
	rm -f -- switch host swctl
help:
	@echo 'make; make clean; make tests; make unit-check; make check; make tsan-check; make valgrind-check; make benchmark'

audit:
	$(PYTHON) tools/audit_returns.py

report:
	$(PYTHON) tools/build_report.py
