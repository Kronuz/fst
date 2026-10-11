cmake_minimum_required(VERSION 3.14)
project(fst LANGUAGES CXX)

# Header-only: fst.hh is the whole implementation.
add_library(fst INTERFACE)
target_include_directories(fst INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(fst INTERFACE cxx_std_20)
add_library(fst::fst ALIAS fst)

# Tests and benchmarks build only when this repo is the top-level project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(fst_test test/test.cc)
  target_link_libraries(fst_test PRIVATE fst)
  add_test(NAME fst COMMAND fst_test)
  add_executable(fst_bench bench/bench.cc)
  target_link_libraries(fst_bench PRIVATE fst)
endif()
