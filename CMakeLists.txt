cmake_minimum_required(VERSION 3.14)
project(fst LANGUAGES CXX)
find_package(Threads REQUIRED)

# Header-only: fst.hh is the whole implementation.
add_library(fst INTERFACE)
target_include_directories(fst INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(fst INTERFACE cxx_std_20)
add_library(fst::fst ALIAS fst)

# Tests and benchmarks build only when this repo is the top-level project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(fst_test test/test.cc)
  target_link_libraries(fst_test PRIVATE fst Threads::Threads)
  target_compile_definitions(fst_test PRIVATE FST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
  add_test(NAME fst COMMAND fst_test)
  add_executable(fst_diff test/diff.cc)
  target_link_libraries(fst_diff PRIVATE fst)
  target_include_directories(fst_diff PRIVATE test)
  add_test(NAME fst_diff COMMAND fst_diff)
  # The same builder with every node hashing alike, so the registry's same_node() decides each hit.
  add_executable(fst_collide test/collide.cc)
  target_link_libraries(fst_collide PRIVATE fst)
  target_include_directories(fst_collide PRIVATE test)
  target_compile_definitions(fst_collide PRIVATE FST_TEST_HASH_MASK=0)
  add_test(NAME fst_collide COMMAND fst_collide)
  # The same builder with the registry's 16-byte slots (used from 4 GiB of output) taking over
  # at 70 KB, before, during and after the larger sets, so the switch and the wide path run
  # against the reference.
  add_executable(fst_diff_wide test/diff.cc)
  target_link_libraries(fst_diff_wide PRIVATE fst)
  target_include_directories(fst_diff_wide PRIVATE test)
  target_compile_definitions(fst_diff_wide PRIVATE FST_TEST_NARROW_LIMIT=70000)
  add_test(NAME fst_diff_wide COMMAND fst_diff_wide)
  # Peak heap of the builder against the reference builder over a sweep of key counts.
  add_executable(fst_memory test/memory.cc)
  target_link_libraries(fst_memory PRIVATE fst)
  target_include_directories(fst_memory PRIVATE test)
  add_test(NAME fst_memory COMMAND fst_memory)
  # libFuzzer target (Clang): cmake -DFST_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
  option(FST_FUZZ "Build the libFuzzer target" OFF)
  if(FST_FUZZ)
    add_executable(fst_fuzz fuzz/fuzz_map.cc)
    target_link_libraries(fst_fuzz PRIVATE fst)
    target_compile_options(fst_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(fst_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  endif()
  add_executable(fst_cmp_bench bench/cmp_bench.cc)
  target_link_libraries(fst_cmp_bench PRIVATE fst)
  target_include_directories(fst_cmp_bench PRIVATE test)
  add_executable(fst_bench bench/bench.cc)
  target_link_libraries(fst_bench PRIVATE fst)
endif()
