#!/bin/sh

set -e

usage() {
  echo "Usage: build.sh [deps|lib|example|test|all]"
  echo "  deps     build vendored third-party static libraries"
  echo "  lib      build libcertain.a into build/lib/"
  echo "  example  build server and client into build/bin/"
  echo "  test     build and run unit tests"
  echo "  all      deps + example + test"
  exit 1
}

if [ $# != 1 ]; then
  usage
fi

cd "$(dirname "$0")"

build_deps() {
  git submodule update --init --recursive
  sh third/autobuild.sh
}

configure() {
  cmake -S . -B build
}

case "$1" in
  deps)
    build_deps
    ;;
  lib)
    configure
    cmake --build build --target certain -j"$(nproc 2>/dev/null || echo 4)"
    ;;
  example)
    build_deps
    configure
    cmake --build build --target server client -j"$(nproc 2>/dev/null || echo 4)"
    ;;
  test)
    configure
    cmake --build build -j"$(nproc 2>/dev/null || echo 4)"
    (cd build && ctest --output-on-failure)
    ;;
  all)
    build_deps
    configure
    cmake --build build -j"$(nproc 2>/dev/null || echo 4)"
    (cd build && ctest --output-on-failure)
    ;;
  *)
    usage
    ;;
esac
