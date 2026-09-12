cmake_minimum_required(VERSION 3.10)
project(paxosstore)

# Two independent consensus implementations live in this repo:
#   certain/  — PaxosLog + DB (built here)
#   paxoskv/  — PaxosLog-as-value (has its own CMakeLists.txt; build separately)
option(PAXOSSTORE_BUILD_CERTAIN "Build the Certain consensus library" ON)

if(PAXOSSTORE_BUILD_CERTAIN)
  add_subdirectory(certain)
endif()
