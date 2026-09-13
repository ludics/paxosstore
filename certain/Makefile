# Certain build wrapper. The real build is CMake; objects and binaries go to
# $(BUILD_DIR)/ (never next to .cc). The 2022 Makefile wrote *.o and *_test
# beside sources; use `make clean-legacy` / repo-root `make clean` to drop those.
#
#   make            # lib + example server/client
#   make lib        # libcertain.a in build/lib
#   make example    # server + client
#   make all_tests
#   make run_all_tests
#   make clean-legacy   # in-tree leftovers only (keeps build/)
#   make clean          # leftover + cmake --target clean
#   make distclean      # leftover + delete build/

BUILD_DIR ?= build
CMAKE ?= cmake
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

.DEFAULT_GOAL := all

.PHONY: all configure cmake lib server client example \
        task_perf plog_worker_perf dump_entry \
        all_tests run_all_tests \
        utils_tests network_tests default_tests src_tests tiny_rpc_tests \
        run_utils_tests run_network_tests run_default_tests run_src_tests \
        run_tiny_rpc_tests clean-legacy clean distclean

configure $(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR)

cmake: $(BUILD_DIR)/CMakeCache.txt

all: example

lib: cmake
	$(CMAKE) --build $(BUILD_DIR) --target certain -j$(JOBS)

server: cmake
	$(CMAKE) --build $(BUILD_DIR) --target server -j$(JOBS)

client: cmake
	$(CMAKE) --build $(BUILD_DIR) --target client -j$(JOBS)

example: cmake
	$(CMAKE) --build $(BUILD_DIR) --target server client -j$(JOBS)

task_perf: cmake
	$(CMAKE) --build $(BUILD_DIR) --target task_perf -j$(JOBS)

plog_worker_perf: cmake
	$(CMAKE) --build $(BUILD_DIR) --target plog_worker_perf -j$(JOBS)

dump_entry: cmake
	$(CMAKE) --build $(BUILD_DIR) --target dump_entry -j$(JOBS)

all_tests: cmake
	$(CMAKE) --build $(BUILD_DIR) --target all -j$(JOBS)

utils_tests network_tests default_tests src_tests tiny_rpc_tests: all_tests

run_all_tests: all_tests
	cd $(BUILD_DIR) && ctest --output-on-failure

run_utils_tests run_network_tests run_default_tests run_src_tests run_tiny_rpc_tests: run_all_tests

# In-tree leftovers from the old Makefile (*.o next to *.cc, 100MB+ test bins).
# Never walk third/ (vendored static libs) or build/ (CMake tree).
clean-legacy:
	@echo "Removing in-tree objects and leftover binaries (keeping $(BUILD_DIR)/)..."
	-find . \( -path ./build -o -path ./third -o -path ./.git \) -prune -o \
		-type f \( -name '*.o' -o -name '*.a' -o -name '*.gcda' -o -name '*.gcno' \
			-o -name '*.gcov' -o -name '*.pb.cc' -o -name '*.pb.h' \) -print \
		| xargs -r rm -f
	-find . \( -path ./build -o -path ./third -o -path ./.git \) -prune -o \
		-type f \( -name '*_test' -o -name 'server' -o -name 'client' \
			-o -name 'task_perf' -o -name 'plog_worker_perf' \
			-o -name 'dump_entry' -o -name 'inspect_plog' \
			-o -name 'benchmark_client' -o -name 'tinyrpc_bench' \) \
		! -name '*.cc' ! -name '*.h' ! -name '*.sh' -print \
		| xargs -r rm -f
	rm -rf test_plog.o test_log.o test_db.o test.cov.root
	rm -f test_cov.info* test_log.o.*

clean: clean-legacy
	@if [ -d "$(BUILD_DIR)" ]; then \
		$(CMAKE) --build $(BUILD_DIR) --target clean; \
	fi

distclean: clean-legacy
	rm -rf $(BUILD_DIR)
