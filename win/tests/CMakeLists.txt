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

add_executable(openpnp-capture-test main.cpp)

add_executable(openpnp-capture-raw-test raw_test.cpp)

add_executable(openpnp-capture-jpeg-decode-bench jpeg_decode_bench.cpp)

add_executable(diagnostic_settings diagnostic_settings.cpp)

add_executable(availability_test availability_test.cpp)

add_executable(inuse_test inuse_test.cpp)

add_executable(fps_test fps_test.cpp)

target_link_libraries(openpnp-capture-test openpnp-capture)
target_link_libraries(openpnp-capture-raw-test openpnp-capture)
target_link_libraries(openpnp-capture-jpeg-decode-bench openpnp-capture)
target_link_libraries(diagnostic_settings openpnp-capture)
target_link_libraries(availability_test openpnp-capture)
target_link_libraries(inuse_test openpnp-capture)
target_link_libraries(fps_test openpnp-capture)