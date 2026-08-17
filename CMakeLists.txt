cmake_minimum_required(VERSION 3.22)
include(CheckLanguage)
project(bine C CXX)

# ============================================================================
# BINE v2 — Binary Interaction Net Executor
# Platforms:
#   - Linux / Windows, CPU-only (BINE_CPU_ONLY=ON or CUDA not found)
#   - Linux / Windows, CUDA (Blackwell sm_120 default)
# ============================================================================

option(BINE_CPU_ONLY "Build without CUDA (pure host implementation)" OFF)
option(BINE_BUILD_TESTS "Build the test suite" ON)
option(BINE_ENABLE_GPU_ICOR "Expose GPU-ICOR/STRAND-P CUDA code (requires CUDA)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 120 CACHE STRING "CUDA architectures (default Blackwell sm_120)")
endif()

set(BINE_HAS_CUDA OFF)
if(NOT BINE_CPU_ONLY AND BINE_ENABLE_GPU_ICOR)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        # nvcc on Windows misparses bare /W3-style MSVC host flags as input
        # files ("A single input file is required ..."), and the NMake/Makefile
        # generators drop the quotes around -Xcompiler="a b c".  Override the
        # platform defaults with per-token -Xcompiler=<flag> entries.
        if(WIN32 AND CMAKE_GENERATOR MATCHES "Makefiles")
            set(CMAKE_CUDA_FLAGS "-D_WINDOWS -Xcompiler=/EHsc" CACHE STRING "" FORCE)
            set(CMAKE_CUDA_FLAGS_RELEASE "-Xcompiler=/O2 -Xcompiler=/Ob2 -DNDEBUG" CACHE STRING "" FORCE)
            set(CMAKE_CUDA_FLAGS_DEBUG "-Xcompiler=/Zi -Xcompiler=/Ob0 -Xcompiler=/Od" CACHE STRING "" FORCE)
            set(CMAKE_CUDA_FLAGS_RELWITHDEBINFO "-Xcompiler=/Zi -Xcompiler=/O2 -Xcompiler=/Ob1 -DNDEBUG" CACHE STRING "" FORCE)
            set(CMAKE_CUDA_FLAGS_MINSIZEREL "-Xcompiler=/O1 -Xcompiler=/Ob1 -DNDEBUG" CACHE STRING "" FORCE)
        endif()
        find_library(CUDART_LIBRARY cudart
            HINTS
                ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES}
                /opt/cuda/lib64
                /opt/cuda/targets/x86_64-linux/lib
                "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.3/lib/x64"
        )
        if(CUDART_LIBRARY)
            set(BINE_HAS_CUDA ON)
        else()
            message(WARNING "CUDA compiler found but cudart library not found; falling back to CPU-only build")
            enable_language(CUDA)  # keep enabled if found
            set(BINE_HAS_CUDA OFF)
        endif()
    endif()
endif()

message(STATUS "BINE: CUDA available: ${BINE_HAS_CUDA}")

# Common include dirs
set(BINE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ============================================================================
# Portable core (always built): compat shims, pure-binary (.bbf) pipeline,
# grammar, exact-math, DFG.
# ============================================================================

set(BINE_CORE_SOURCES
    src/compat/bine_compat.c
    src/format/bbf.c
    src/logic/diagonal.c
    src/compress/zentropy.c
    src/optimizer/kolmogorov.c
    src/optimizer/dfg.c
    src/grammar/apex.c
    src/grammar/repair.c
    src/eml/eml.c
    src/exec/cpu_bridge.c
)

# gpu_runner.cu / universal.cu carry #ifndef __CUDACC__ fallbacks: compile as
# CUDA when the toolkit is present, otherwise as C++ for the host-only build.
if(BINE_HAS_CUDA)
    set(BINE_CORE_SOURCES ${BINE_CORE_SOURCES} src/exec/gpu_runner.cu src/optimizer/universal.cu)
else()
    set(BINE_CORE_SOURCES ${BINE_CORE_SOURCES} src/exec/gpu_runner.cu src/optimizer/universal.cu)
    set_source_files_properties(src/exec/gpu_runner.cu src/optimizer/universal.cu
        PROPERTIES LANGUAGE CXX)
endif()

add_library(bine_core STATIC ${BINE_CORE_SOURCES})
target_include_directories(bine_core PUBLIC ${BINE_INCLUDE_DIRS})
if(BINE_HAS_CUDA)
    target_include_directories(bine_core PUBLIC ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
endif()
target_compile_definitions(bine_core PUBLIC BINE_BUILD_GPU=$<BOOL:${BINE_HAS_CUDA}>)

# ============================================================================
# ELF pipeline (always built, host code): binary analysis, ELF parser,
# optimizer passes, serialization, STRAND masks, ICOR host reduction.
# ============================================================================

set(BINE_ELF_SOURCES
    src/binary/decode.c
    src/binary/decode_arm.c
    src/binary/entropy.c
    src/binary/length.c
    src/binary/pattern.c
    src/cache.c
    src/elf/api.c
    src/elf/intrinsics.c
    src/elf/parser.c
    src/optimizer/cfg.c
    src/optimizer/gpu_cfg.c
    src/optimizer/gpu_dfg.c
    src/optimizer/gpu_net.c
    src/optimizer/merge.c
    src/optimizer/net_build.c
    src/optimizer/nop.c
    src/optimizer/peephole.c
    src/optimizer/reorder.c
    src/serialize/compact.c
    src/serialize/encode.c
    src/serialize/encode_arm.c
    src/serialize/encode_riscv.c
    src/serialize/icor_to_binary.c
    src/serialize/layout.c
    src/serialize/net_to_elf.c
    src/serialize/reloc.c
    src/strand/mask.c
    src/strand/strand_p.c
    src/icor/reduce_cpu.c
)

add_library(bine_elf STATIC ${BINE_ELF_SOURCES})
target_include_directories(bine_elf PUBLIC ${BINE_INCLUDE_DIRS})
target_link_libraries(bine_elf PUBLIC bine_core)

# ============================================================================
# GPU engines (built only when CUDA is available)
# ============================================================================

if(BINE_HAS_CUDA)
    set(BINE_GPU_SOURCES
        src/icor/wrapper.cu
        src/icor/reduce.cu
        src/icor/kernels_impl.cu
        src/strand/schedule.cu
        src/strand/pbits.cu
        src/optimizer/reg_assign.cu
    )
    add_library(bine_gpu STATIC ${BINE_GPU_SOURCES})
    target_include_directories(bine_gpu PUBLIC ${BINE_INCLUDE_DIRS} ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
    # .cuh headers include siblings across src/icor and src/strand by bare name.
    target_include_directories(bine_gpu PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/icor
        ${CMAKE_CURRENT_SOURCE_DIR}/src/strand)
    target_compile_definitions(bine_gpu PUBLIC BINE_BUILD_GPU=1)
    set_source_files_properties(${BINE_GPU_SOURCES} PROPERTIES COMPILE_OPTIONS "--generate-line-info;--maxrregcount=48;--use_fast_math")
endif()

# ============================================================================
# Compile options (per-toolchain)
# ============================================================================

foreach(tgt bine_core bine_elf)
    if(MSVC)
        # C/CXX only: nvcc must not receive bare /W3-style host flags.
        target_compile_options(${tgt} PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /O2 /wd4996>)
    else()
        target_compile_options(${tgt} PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
        if(NOT BINE_HAS_CUDA)
            # -march=native is fine for host-only builds; keep baseline when CUDA
            # is present because nvcc host passes must match the C compiler ABI.
            target_compile_options(${tgt} PRIVATE -march=native)
        endif()
    endif()
endforeach()

if(MSVC)
    target_compile_options(bine_core PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/EHsc>)
endif()

# ============================================================================
# Main executable (unified CLI: ELF + .bbf pipelines)
# ============================================================================

add_executable(bine src/bine.c)
target_link_libraries(bine PRIVATE bine_elf)
if(BINE_HAS_CUDA)
    target_link_libraries(bine PRIVATE bine_gpu ${CUDART_LIBRARY})
endif()
if(MSVC)
    target_compile_options(bine PRIVATE /W3 /O2 /wd4996)
else()
    target_compile_options(bine PRIVATE -O3 -Wall -Wno-unused-parameter)
endif()
if(NOT WIN32)
    target_link_libraries(bine PRIVATE m)
endif()
target_compile_definitions(bine PRIVATE BINE_COMBINED_TESTS=1 BINE_BUILD_GPU=$<BOOL:${BINE_HAS_CUDA}>)

# ============================================================================
# Tests
# ============================================================================

if(BINE_BUILD_TESTS)
    enable_testing()

    set(BINE_TESTS
        tests/test_suite.c
        tests/test_bbf.c
        tests/test_diagonal.c
        tests/test_zentropy.c
        tests/test_kolmogorov.c
        tests/test_dfg.c
        tests/test_pure_binary.c
        tests/test_elf_pipeline.c
        tests/test_serialize.c
        tests/test_bbf_fuzz.c
        tests/test_compat.c
        tests/test_layout.c
    )
    if(BINE_HAS_CUDA)
        list(APPEND BINE_TESTS tests/test_gpu.c)
    endif()

    add_executable(bine_tests ${BINE_TESTS})
    target_link_libraries(bine_tests PRIVATE bine_elf)
    if(BINE_HAS_CUDA)
        target_link_libraries(bine_tests PRIVATE bine_gpu ${CUDART_LIBRARY})
    endif()
    if(MSVC)
        target_compile_options(bine_tests PRIVATE /W3 /O2 /wd4996)
    else()
        target_compile_options(bine_tests PRIVATE -O3 -Wall -Wno-unused-parameter)
    endif()
    if(NOT WIN32)
        target_link_libraries(bine_tests PRIVATE m)
    endif()
    target_compile_definitions(bine_tests PRIVATE BINE_COMBINED_TESTS=1 BINE_BUILD_GPU=$<BOOL:${BINE_HAS_CUDA}>)
    add_test(NAME bine_tests COMMAND bine_tests)

    # Example .bbf generator
    add_executable(gen_examples tests/gen_examples.c)
    target_link_libraries(gen_examples PRIVATE bine_elf)
    if(BINE_HAS_CUDA)
        target_link_libraries(gen_examples PRIVATE bine_gpu ${CUDART_LIBRARY})
    endif()
    if(NOT WIN32)
        target_link_libraries(gen_examples PRIVATE m)
    endif()
endif()

# ============================================================================
# Install
# ============================================================================

include(GNUInstallDirs)
install(TARGETS bine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/bine.h include/bine_internal.h include/bine_compat.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bine)
