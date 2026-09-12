# Certain build wrapper. The real build is CMake; binaries go to
# $(BUILD_DIR)/bin and libraries to $(BUILD_DIR)/lib.
#
#   make            # lib + example server/client
#   make lib        # libcertain.a
#   make example    # server + client
#   make all_tests
#   make run_all_tests
#   make clean

BUILD_DIR ?= build
CMAKE ?= cmake
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

.DEFAULT_GOAL := all

.PHONY: all configure cmake lib server client example \
        task_perf plog_worker_perf dump_entry \
        all_tests run_all_tests \
        utils_tests network_tests default_tests src_tests tiny_rpc_tests \
        run_utils_tests run_network_tests run_default_tests run_src_tests \
        run_tiny_rpc_tests clean

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

clean:
	rm -rf $(BUILD_DIR)
	rm -f libcertain.a server client task_perf plog_worker_perf
	rm -f proto/*.pb.cc proto/*.pb.h proto/*.pb.o proto/*.o
	rm -f utils/*.o network/*.o src/*.o default/*.o tiny_rpc/*.o tools/*.o example/*.o
	rm -f utils/*_test network/*_test src/*_test default/*_test tiny_rpc/*_test
	rm -rf test_plog.o test_log.o test_db.o test.cov.root test_cov.info*
	rm -f */*.gcda */*.gcno *.gcov
