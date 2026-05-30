cmake_minimum_required(VERSION 3.22)
project(bine C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CUDA_STANDARD 17)

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 120 CACHE STRING "CUDA architectures (default Blackwell sm_120)")
endif()

if(NOT CMAKE_CUDA_HOST_COMPILER)
    find_program(DEFAULT_CUDA_HOST_COMPILER NAMES g++)
    if(DEFAULT_CUDA_HOST_COMPILER)
        set(CMAKE_CUDA_HOST_COMPILER ${DEFAULT_CUDA_HOST_COMPILER} CACHE FILEPATH "CUDA host compiler")
    endif()
endif()

enable_language(CUDA)

enable_testing()

find_library(CUDART_LIBRARY cudart
    HINTS
        ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES}
        /opt/cuda/lib64
        /opt/cuda/targets/x86_64-linux/lib
    REQUIRED
)

set(BINE_SOURCES
    src/main.c
    src/cache.c
    src/elf/parser.c
    src/elf/intrinsics.c
    src/elf/api.c
    src/binary/decode.c
    src/binary/entropy.c
    src/binary/pattern.c
    src/grammar/apex.c
    src/grammar/repair.c
    src/eml/eml.c
    src/strand/mask.c
    src/strand/pbits.cu
    src/strand/schedule.cu
    src/icor/io.c
    src/icor/reduce.cu
    src/icor/kernels_impl.cu
    src/icor/wrapper.cu
    src/optimizer/cfg.c
    src/optimizer/net_build.c
    src/optimizer/binary_to_icor.c
    src/optimizer/nop.c
    src/optimizer/merge.c
    src/optimizer/reorder.c
    src/optimizer/peephole.c
    src/optimizer/reg_assign.cu
    src/serialize/encode.c
    src/serialize/net_to_elf.c
    src/serialize/icor_to_binary.c
    src/serialize/compact.c
    src/serialize/reloc.c
)

add_executable(bine ${BINE_SOURCES})

target_include_directories(bine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/icor
    ${CMAKE_CURRENT_SOURCE_DIR}/src/strand
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
)

target_compile_options(bine PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:
        --generate-line-info
        --maxrregcount=48
        -Xptxas=-v
        --use_fast_math
    >
    $<$<COMPILE_LANGUAGE:C>:
        -O3 -march=native
    >
)

target_link_libraries(bine PRIVATE ${CUDART_LIBRARY} m)

# Test targets
add_executable(test_pipeline
    tests/test_pipeline.c
    src/elf/parser.c
    src/elf/intrinsics.c
    src/elf/api.c
    src/cache.c
    src/binary/decode.c
    src/binary/entropy.c
    src/binary/pattern.c
    src/grammar/apex.c
    src/grammar/repair.c
    src/eml/eml.c
    src/strand/mask.c
    src/strand/pbits.cu
    src/strand/schedule.cu
    src/icor/io.c
    src/icor/reduce.cu
    src/icor/kernels_impl.cu
    src/icor/wrapper.cu
    src/optimizer/cfg.c
    src/optimizer/net_build.c
    src/optimizer/binary_to_icor.c
    src/optimizer/nop.c
    src/optimizer/merge.c
    src/optimizer/reorder.c
    src/optimizer/peephole.c
    src/optimizer/reg_assign.cu
    src/serialize/encode.c
    src/serialize/net_to_elf.c
    src/serialize/icor_to_binary.c
    src/serialize/compact.c
    src/serialize/reloc.c
)
target_include_directories(test_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/icor
    ${CMAKE_CURRENT_SOURCE_DIR}/src/strand
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
)
target_compile_options(test_pipeline PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:
        --generate-line-info
        --maxrregcount=48
        -Xptxas=-v
        --use_fast_math
    >
    $<$<COMPILE_LANGUAGE:C>:
        -O3 -march=native
    >
)
target_link_libraries(test_pipeline PRIVATE ${CUDART_LIBRARY} m)

add_test(NAME bine_pipeline COMMAND test_pipeline)
