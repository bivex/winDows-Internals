cmake_minimum_required(VERSION 3.20)
project(uaf_poc C)

set(CMAKE_C_STANDARD 11)

# ── Sources ────────────────────────────────────────────────────────────────────
set(SOURCES uaf_poc.c)

# ── Targets ───────────────────────────────────────────────────────────────────

# 1. Raw binary (no instrumentation) — shows UAF succeeds silently
add_executable(uaf_poc_raw ${SOURCES})

# 2. ASan-instrumented binary — shows UAF detected at runtime
add_executable(uaf_poc_asan ${SOURCES})

# ── Compiler flags ─────────────────────────────────────────────────────────────
if(MSVC)
    # MSVC / clang-cl (Windows ARM64 or x64)
    # /Zi  = full debug info (PDB)
    # /Od  = no optimization (keeps source lines 1:1 with asm for WinDbg)
    # /DEBUG = emit .pdb linker output
    target_compile_options(uaf_poc_raw  PRIVATE /W4 /Zi /Od /GS-)
    target_link_options   (uaf_poc_raw  PRIVATE /DEBUG /INCREMENTAL:NO)

    target_compile_options(uaf_poc_asan PRIVATE /W4 /Zi /Od /fsanitize=address)
    target_link_options   (uaf_poc_asan PRIVATE /DEBUG /INCREMENTAL:NO)

    # MSVC ASan runtime — must be dynamic CRT
    set_target_properties(uaf_poc_asan PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_target_properties(uaf_poc_raw PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    # Clang / GCC (macOS, Linux, or MinGW cross-compile)
    target_compile_options(uaf_poc_raw  PRIVATE -Wall -Wextra -g -O0)
    target_compile_options(uaf_poc_asan PRIVATE -Wall -Wextra -g -O0
                                                -fsanitize=address
                                                -fno-omit-frame-pointer)
    target_link_options   (uaf_poc_asan PRIVATE -fsanitize=address)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS uaf_poc_raw uaf_poc_asan
        RUNTIME DESTINATION bin)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "──────────────────────────────────────────")
message(STATUS "  UAF PoC build")
message(STATUS "  Compiler  : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  System    : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Targets   : uaf_poc_raw  (no ASan)")
message(STATUS "              uaf_poc_asan (AddressSanitizer)")
message(STATUS "──────────────────────────────────────────")
