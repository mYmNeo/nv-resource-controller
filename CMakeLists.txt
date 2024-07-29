cmake_minimum_required(VERSION 3.20.1 FATAL_ERROR)

project(
  CUDA_HOOK
  VERSION 0.1.0
  LANGUAGES C
)

option(NV_KERNEL_VERSION "NVIDIA kernel module version")
if(NOT NV_KERNEL_VERSION)
  message(
    FATAL_ERROR
      "NV_KERNEL_VERSION not set, available version can be found at https://github.com/NVIDIA/open-gpu-kernel-modules"
  )
endif()

if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${NV_KERNEL_VERSION}.tar.gz)
  file(
    DOWNLOAD
    https://github.com/NVIDIA/open-gpu-kernel-modules/archive/refs/tags/${NV_KERNEL_VERSION}.tar.gz
    ${CMAKE_CURRENT_BINARY_DIR}/${NV_KERNEL_VERSION}.tar.gz
  )
endif()

file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_BINARY_DIR}/${NV_KERNEL_VERSION}.tar.gz DESTINATION
     ${CMAKE_CURRENT_BINARY_DIR}
)

set(CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -fvisibility=hidden -Wunused -Werror -Wno-deprecated-non-prototype -D_GNU_SOURCE"
)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -s")

find_package(Threads REQUIRED)
add_library(
  cuda_hook SHARED src/dlfcn.c src/entry.c src/cuda_hook.c src/ioctl_hook.c src/util.c src/env.c
)

add_compile_definitions(LIBRARY_NAME="$<TARGET_FILE_NAME:cuda_hook>")

target_include_directories(
  cuda_hook
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/open-gpu-kernel-modules-${NV_KERNEL_VERSION}/src/nvidia>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/open-gpu-kernel-modules-${NV_KERNEL_VERSION}/src/common/sdk/nvidia/inc>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/open-gpu-kernel-modules-${NV_KERNEL_VERSION}/src/nvidia/inc/kernel>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/open-gpu-kernel-modules-${NV_KERNEL_VERSION}/src/nvidia/arch/nvalloc/unix/include>"
)

target_link_libraries(cuda_hook PRIVATE ${CMAKE_DL_LIBS} Threads::Threads -lrt)

add_custom_target(server)
find_library(LIB_RT rt REQUIRED)
add_executable(server_monitor src/server_monitor.c src/util.c)
target_include_directories(
  server_monitor PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(server_monitor PRIVATE ${CMAKE_DL_LIBS} Threads::Threads -lrt -lm)

add_dependencies(server server_monitor)

install(TARGETS cuda_hook DESTINATION lib)
