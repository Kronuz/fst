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
  target_compile_definitions(fst_test PRIVATE FST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
  add_test(NAME fst COMMAND fst_test)
  # libFuzzer target (Clang): cmake -DFST_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
  option(FST_FUZZ "Build the libFuzzer target" OFF)
  if(FST_FUZZ)
    add_executable(fst_fuzz fuzz/fuzz_map.cc)
    target_link_libraries(fst_fuzz PRIVATE fst)
    target_compile_options(fst_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(fst_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  endif()
  add_executable(fst_bench bench/bench.cc)
  target_link_libraries(fst_bench PRIVATE fst)
endif()
