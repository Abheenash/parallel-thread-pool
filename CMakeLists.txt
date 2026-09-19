cmake_minimum_required(VERSION 3.14)
project(parallel_thread_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TP_SANITIZE "Build tests with -fsanitize=<thread|address>" "")
option(TP_BUILD_BENCH "Build the benchmark binary" ON)
option(TP_BUILD_TESTS "Build the tests" ON)

find_package(Threads REQUIRED)

# The library is header-only.
add_library(tp INTERFACE)
add_library(tp::tp ALIAS tp)
target_include_directories(tp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tp INTERFACE Threads::Threads)
target_compile_features(tp INTERFACE cxx_std_17)

function(tp_warnings tgt)
  target_compile_options(${tgt} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow>)
  if(TP_SANITIZE)
    target_compile_options(${tgt} PRIVATE -fsanitize=${TP_SANITIZE} -g -O1 -fno-omit-frame-pointer)
    target_link_options(${tgt} PRIVATE -fsanitize=${TP_SANITIZE})
  endif()
endfunction()

add_executable(example src/example.cpp)
target_link_libraries(example PRIVATE tp::tp)
tp_warnings(example)

if(TP_BUILD_BENCH)
  add_executable(bench bench/bench.cpp)
  target_link_libraries(bench PRIVATE tp::tp)
  target_compile_options(bench PRIVATE -O3)
  tp_warnings(bench)
endif()

if(TP_BUILD_TESTS)
  enable_testing()
  add_executable(test_thread_pool tests/test_thread_pool.cpp)
  target_link_libraries(test_thread_pool PRIVATE tp::tp)
  tp_warnings(test_thread_pool)
  add_test(NAME thread_pool COMMAND test_thread_pool)
  set_tests_properties(thread_pool PROPERTIES TIMEOUT 300)
endif()
