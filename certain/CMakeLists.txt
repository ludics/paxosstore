cmake_minimum_required(VERSION 3.10)
project(certain CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# The original Makefile used -O2 -g without -DNDEBUG. Many tests and some
# library paths rely on assert() actually running (e.g. assert(sock.Init())).
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO
  "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "-DNDEBUG" "" CMAKE_C_FLAGS_RELWITHDEBINFO
  "${CMAKE_C_FLAGS_RELWITHDEBINFO}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(CERTAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(CERTAIN_THIRD_DIR ${CERTAIN_DIR}/third)

option(CERTAIN_BUILD_TESTS "Build Certain unit tests" ON)
option(CERTAIN_BUILD_EXAMPLES "Build Certain example server/client" ON)
option(CERTAIN_ENABLE_TCMALLOC
  "Link gperftools tcmalloc (crashes on modern glibc+libco+libunwind)" OFF)
option(CERTAIN_ENABLE_PROFILE "Define CERTAIN_PROFILE (gperftools)" OFF)
option(CERTAIN_ENABLE_COVERAGE "Build with gcov coverage flags" OFF)

#------------------------------------------------------------------------------
# Vendored third-party static libraries (built by third/autobuild.sh)
#------------------------------------------------------------------------------
function(certain_require_file var path hint)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR
      "Missing ${hint}:\n  ${path}\n"
      "Build third-party deps first:\n"
      "  cd ${CERTAIN_DIR} && sh third/autobuild.sh")
  endif()
  set(${var} "${path}" PARENT_SCOPE)
endfunction()

certain_require_file(PROTOC
  ${CERTAIN_THIRD_DIR}/protobuf/src/protoc "protoc")
certain_require_file(PROTOBUF_A
  ${CERTAIN_THIRD_DIR}/protobuf/src/.libs/libprotobuf.a "libprotobuf")
certain_require_file(ROCKSDB_A
  ${CERTAIN_THIRD_DIR}/rocksdb/librocksdb.a "librocksdb")
certain_require_file(COLIB_A
  ${CERTAIN_THIRD_DIR}/libco/lib/libcolib.a "libcolib")
certain_require_file(GTEST_A
  ${CERTAIN_THIRD_DIR}/googletest/googletest/libgtest.a "libgtest")
certain_require_file(GFLAGS_A
  ${CERTAIN_THIRD_DIR}/gflags/lib/libgflags.a "libgflags")
certain_require_file(GLOG_A
  ${CERTAIN_THIRD_DIR}/glog/.libs/libglog.a "libglog")
if(CERTAIN_ENABLE_TCMALLOC)
  certain_require_file(TCMALLOC_A
    ${CERTAIN_THIRD_DIR}/gperftools/.libs/libtcmalloc_and_profiler.a
    "libtcmalloc_and_profiler")
endif()

if(EXISTS "/usr/local/lib/libsnappy.a")
  set(SNAPPY_A /usr/local/lib/libsnappy.a)
elseif(EXISTS "/usr/lib/x86_64-linux-gnu/libsnappy.a")
  set(SNAPPY_A /usr/lib/x86_64-linux-gnu/libsnappy.a)
else()
  find_library(SNAPPY_A NAMES snappy)
endif()
if(NOT SNAPPY_A)
  message(FATAL_ERROR "libsnappy not found (needed by RocksDB)")
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_library(UNWIND_LIBRARY NAMES unwind)

#------------------------------------------------------------------------------
# Compile flags (match the historical Makefile, plus modern GCC relaxations)
#------------------------------------------------------------------------------
add_compile_options(-Wall -Werror -Wno-format)
add_compile_options(
  -Wno-error=sign-compare
  -Wno-error=deprecated-declarations
  -Wno-error=unused-result
  -Wno-error=unused-variable
  -Wno-error=unused-but-set-variable
  -Wno-error=unused-function
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 7)
    add_compile_options(-faligned-new)
  endif()
  add_compile_options(
    -Wno-error=deprecated-copy
    -Wno-error=class-memaccess
    -Wno-error=catch-value
    -Wno-error=maybe-uninitialized
    -Wno-error=stringop-overflow
    -Wno-error=stringop-truncation
    -Wno-error=format-truncation
    -Wno-error=restrict
    -Wno-error=type-limits
    -Wno-error=implicit-fallthrough
    -Wno-error=pessimizing-move
  )
endif()

add_compile_definitions(SNAPPY)
if(CERTAIN_ENABLE_PROFILE)
  add_compile_definitions(CERTAIN_PROFILE)
endif()

if(CERTAIN_ENABLE_COVERAGE)
  add_compile_options(-fprofile-arcs -ftest-coverage -O0)
  add_link_options(-fprofile-arcs -ftest-coverage)
endif()

#------------------------------------------------------------------------------
# Protobuf generation (out-of-tree, so source proto/ stays clean)
#------------------------------------------------------------------------------
set(CERTAIN_PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR})
file(MAKE_DIRECTORY ${CERTAIN_PROTO_OUT}/proto)

function(certain_gen_proto name)
  set(src ${CERTAIN_DIR}/proto/${name}.proto)
  set(cc ${CERTAIN_PROTO_OUT}/proto/${name}.pb.cc)
  set(h ${CERTAIN_PROTO_OUT}/proto/${name}.pb.h)
  add_custom_command(
    OUTPUT ${cc} ${h}
    COMMAND ${PROTOC} --cpp_out=${CERTAIN_PROTO_OUT} -I${CERTAIN_DIR} ${src}
    DEPENDS ${src}
    COMMENT "Generating proto/${name}.pb.cc"
    VERBATIM)
  set(CERTAIN_${name}_PROTO_SRCS ${cc} PARENT_SCOPE)
endfunction()

certain_gen_proto(tiny_rpc)
certain_gen_proto(certain)
certain_gen_proto(tools)

#------------------------------------------------------------------------------
# Third-party link interface
#------------------------------------------------------------------------------
add_library(certain_deps INTERFACE)
target_include_directories(certain_deps INTERFACE
  ${CERTAIN_DIR}
  ${CERTAIN_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CERTAIN_THIRD_DIR}/protobuf/src
  ${CERTAIN_THIRD_DIR}/libco
  ${CERTAIN_THIRD_DIR}/glog/src
  ${CERTAIN_THIRD_DIR}/gflags/include
  ${CERTAIN_THIRD_DIR}/rocksdb/include
  ${CERTAIN_THIRD_DIR}/gperftools/src
)
target_link_libraries(certain_deps INTERFACE
  ${PROTOBUF_A}
  ${GLOG_A}
  ${GFLAGS_A}
  ${ROCKSDB_A}
  ${COLIB_A}
  ${SNAPPY_A}
  Threads::Threads
  ${CMAKE_DL_LIBS}
  ZLIB::ZLIB
)
if(CERTAIN_ENABLE_TCMALLOC)
  target_link_libraries(certain_deps INTERFACE ${TCMALLOC_A})
  if(UNWIND_LIBRARY)
    target_link_libraries(certain_deps INTERFACE ${UNWIND_LIBRARY})
  endif()
endif()

#------------------------------------------------------------------------------
# libcertain
#------------------------------------------------------------------------------
set(CERTAIN_UTILS_SRCS
  utils/crc32.cc
  utils/mem_pool.cc
  utils/thread.cc
  utils/co_lock.cc
)

set(CERTAIN_NETWORK_SRCS
  network/tcp_socket.cc
  network/poller.cc
  network/write_item_list.cc
  network/msg_header.cc
  network/msg_channel.cc
)

set(CERTAIN_SRC_SRCS
  src/conn_mng.cc
  src/conn_worker.cc
  src/command.cc
  src/msg_serialize.cc
  src/notify_helper.cc
  src/libco_notify_helper.cc
  src/async_queue_mng.cc
  src/entry_state.cc
  src/msg_channel_helper.cc
  src/entity_helper.cc
  src/entity_info_mng.cc
  src/entry_info_mng.cc
  src/msg_worker.cc
  src/entity_worker.cc
  src/plog_worker.cc
  src/db_worker.cc
  src/db_limited_worker.cc
  src/recover_worker.cc
  src/catchup_worker.cc
  src/tools_worker.cc
  src/wrapper.cc
  src/certain.cc
)

set(CERTAIN_TINY_RPC_SRCS
  tiny_rpc/tiny_rpc.cc
  tiny_rpc/tiny_client.cc
  tiny_rpc/tiny_server.cc
)

set(CERTAIN_TOOLS_SRCS
  tools/tools_service.cc
)

add_library(certain STATIC
  ${CERTAIN_UTILS_SRCS}
  ${CERTAIN_NETWORK_SRCS}
  ${CERTAIN_SRC_SRCS}
  ${CERTAIN_TINY_RPC_SRCS}
  ${CERTAIN_TOOLS_SRCS}
  ${CERTAIN_tiny_rpc_PROTO_SRCS}
  ${CERTAIN_certain_PROTO_SRCS}
  ${CERTAIN_tools_PROTO_SRCS}
)
target_link_libraries(certain PUBLIC certain_deps)
set_target_properties(certain PROPERTIES OUTPUT_NAME certain)

add_library(certain_default STATIC
  default/route_impl.cc
  default/plog_impl.cc
  default/db_impl.cc
  default/log_impl.cc
  default/tiny_service_impl.cc
)
target_link_libraries(certain_default PUBLIC certain)

#------------------------------------------------------------------------------
# Examples / tools
#------------------------------------------------------------------------------
if(CERTAIN_BUILD_EXAMPLES)
  add_executable(server example/server.cc)
  target_link_libraries(server PRIVATE certain_default)

  add_executable(client example/client.cc)
  target_link_libraries(client PRIVATE certain_default)

  add_executable(task_perf example/task.cc example/task_perf.cc)
  target_link_libraries(task_perf PRIVATE certain_default)

  add_executable(plog_worker_perf src/plog_worker_perf.cc)
  target_link_libraries(plog_worker_perf PRIVATE certain_default)

  add_executable(dump_entry tools/dump_entry.cc)
  target_link_libraries(dump_entry PRIVATE certain_default)

  add_executable(inspect_plog tools/inspect_plog.cc)
  target_link_libraries(inspect_plog PRIVATE certain_default)

  add_executable(benchmark_client tools/benchmark_client.cc)
  target_link_libraries(benchmark_client PRIVATE certain_default)

  add_executable(tinyrpc_bench tools/tinyrpc_bench.cc)
  target_link_libraries(tinyrpc_bench PRIVATE certain_default)

  add_executable(light_list_bench tools/light_list_bench.cc)
  target_include_directories(light_list_bench PRIVATE ${CERTAIN_DIR})
endif()

#------------------------------------------------------------------------------
# Tests
#------------------------------------------------------------------------------
if(CERTAIN_BUILD_TESTS)
  enable_testing()

  add_library(gtest STATIC IMPORTED)
  set_target_properties(gtest PROPERTIES
    IMPORTED_LOCATION ${GTEST_A}
    INTERFACE_INCLUDE_DIRECTORIES ${CERTAIN_THIRD_DIR}/googletest/googletest/include)

  function(certain_add_test src_file)
    get_filename_component(test_name ${src_file} NAME_WE)
    add_executable(${test_name} ${CERTAIN_DIR}/${src_file})
    target_link_libraries(${test_name} PRIVATE certain_default gtest)
    set_target_properties(${test_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)
    set(work_dir ${CMAKE_BINARY_DIR}/test-work/${test_name})
    file(MAKE_DIRECTORY ${work_dir})
    add_test(NAME ${test_name} COMMAND ${test_name} WORKING_DIRECTORY ${work_dir})
  endfunction()

  set(CERTAIN_TEST_SRCS
    utils/crc32_test.cc
    utils/lock_free_queue_test.cc
    utils/light_list_test.cc
    utils/light_list_modern_test.cc
    utils/array_timer_test.cc
    utils/lru_table_test.cc
    utils/mem_pool_test.cc
    utils/thread_test.cc
    utils/hash_test.cc
    utils/time_test.cc
    utils/capacity_limiter_test.cc
    utils/uuid_mng_test.cc
    utils/traffic_limiter_test.cc
    utils/co_lock_test.cc
    network/inet_addr_test.cc
    network/tcp_socket_test.cc
    network/poller_test.cc
    network/write_item_list_test.cc
    network/msg_header_test.cc
    network/msg_channel_test.cc
    default/route_impl_test.cc
    default/plog_impl_test.cc
    default/db_impl_test.cc
    src/conn_mng_test.cc
    src/conn_worker_test.cc
    src/command_test.cc
    src/msg_serialize_test.cc
    src/notify_helper_test.cc
    src/libco_notify_helper_test.cc
    src/async_queue_mng_test.cc
    src/entry_state_test.cc
    src/msg_channel_helper_test.cc
    src/entity_helper_test.cc
    src/entity_info_mng_test.cc
    src/entry_info_mng_test.cc
    src/msg_worker_test.cc
    src/plog_worker_test.cc
    src/db_worker_test.cc
    src/db_limited_worker_test.cc
    src/recover_worker_test.cc
    src/wrapper_test.cc
    src/five_replica_test.cc
    src/entity_worker_test.cc
    tiny_rpc/tiny_rpc_test.cc
  )

  foreach(src ${CERTAIN_TEST_SRCS})
    certain_add_test(${src})
  endforeach()
endif()
