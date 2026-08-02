cmake_minimum_required(VERSION 3.14)
project(FastECCBranchless VERSION 2.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# Examples
add_executable(basic_ecc examples/basic_ecc.cpp)
add_executable(stream_transmission examples/stream_transmission.cpp)
add_executable(ecc_data_transmission_demo examples/ecc_data_transmission_demo.cpp)
add_executable(fast_10mb_transmission_demo examples/fast_10mb_transmission_demo.cpp)
add_executable(ecc_hamming_poc examples/ecc_hamming_poc.cpp)
add_executable(ecc_hamming_fast_poc examples/ecc_hamming_fast_poc.cpp)

# Tests
enable_testing()
add_executable(test_ecc tests/test_ecc.cpp)
add_executable(test_edge_cases tests/test_edge_cases.cpp)
add_executable(test_memleak tests/test_memleak.cpp)
add_executable(test_max_throughput tests/test_max_throughput.cpp)
add_executable(test_simd_ecc tests/test_simd_ecc.cpp)

add_test(NAME test_ecc COMMAND test_ecc)
add_test(NAME test_edge_cases COMMAND test_edge_cases)
add_test(NAME test_memleak COMMAND test_memleak)
add_test(NAME test_max_throughput COMMAND test_max_throughput)
add_test(NAME test_simd_ecc COMMAND test_simd_ecc)
