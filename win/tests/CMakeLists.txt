#
# CMAKE build file for OpenPnP Capture library
#
# This generates make files for several build systems,
# such as GNU Make, Ninja and visual studio
#
# When invoking on Windows systems, make sure the
# compiler (Visual Studio) is in the search path.
#
# Author: Niels A. Moseley
#

cmake_minimum_required(VERSION 3.0)
project (openpnp-capture-test)

set (SOURCE main.cpp ../../common/logging.cpp)

add_executable(openpnp-capture-test ${SOURCE})

set (RAW_SOURCE raw_test.cpp ../../common/logging.cpp)

add_executable(openpnp-capture-raw-test ${RAW_SOURCE})

set (JPEG_DECODE_BENCH_SOURCE jpeg_decode_bench.cpp)

add_executable(openpnp-capture-jpeg-decode-bench ${JPEG_DECODE_BENCH_SOURCE})

target_link_libraries(openpnp-capture-test openpnp-capture)
target_link_libraries(openpnp-capture-raw-test openpnp-capture)
target_link_libraries(openpnp-capture-jpeg-decode-bench openpnp-capture)