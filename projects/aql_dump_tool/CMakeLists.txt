#
# AQL Dump Tool — captures raw AQL packet streams, barrier timestamps,
# and kernel descriptor dumps.
#
# Build:
#   mkdir build && cd build
#   cmake .. -DCMAKE_PREFIX_PATH=/opt/rocm
#   make
#
# Usage:
#   rocprofv3 --tool ./libaql_dump_tool.so -- ./aql_test_app
#   # or
#   LD_PRELOAD=./libaql_dump_tool.so ./aql_test_app
#
# Output: aql_dump_output.json (or set AQL_DUMP_OUTPUT env var)
#

cmake_minimum_required(VERSION 3.21.0 FATAL_ERROR)

# ---------------------------------------------------------------------------
# Project — CXX only (HIP is enabled later for the test app if available)
# ---------------------------------------------------------------------------
project(aql-dump-tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Optionally find HIP compiler for the test application
# ---------------------------------------------------------------------------
if(NOT CMAKE_HIP_COMPILER)
    find_program(
        amdclangpp_EXECUTABLE
        NAMES amdclang++
        HINTS ${ROCM_PATH} ENV ROCM_PATH /opt/rocm
        PATHS ${ROCM_PATH} ENV ROCM_PATH /opt/rocm
        PATH_SUFFIXES bin llvm/bin NO_CACHE)
    mark_as_advanced(amdclangpp_EXECUTABLE)

    if(amdclangpp_EXECUTABLE)
        set(CMAKE_HIP_COMPILER "${amdclangpp_EXECUTABLE}")
    endif()
endif()

set(HIP_AVAILABLE OFF)
if(CMAKE_HIP_COMPILER)
    enable_language(HIP)
    set(HIP_AVAILABLE ON)

    foreach(_TYPE DEBUG MINSIZEREL RELEASE RELWITHDEBINFO)
        if("${CMAKE_HIP_FLAGS_${_TYPE}}" STREQUAL "")
            set(CMAKE_HIP_FLAGS_${_TYPE} "${CMAKE_CXX_FLAGS_${_TYPE}}")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# ROCm paths
# ---------------------------------------------------------------------------
if(NOT DEFINED ROCM_PATH)
    if(DEFINED ENV{ROCM_PATH})
        set(ROCM_PATH "$ENV{ROCM_PATH}")
    else()
        set(ROCM_PATH "/opt/rocm")
    endif()
endif()

# ---------------------------------------------------------------------------
# Find rocprofiler-sdk
# ---------------------------------------------------------------------------
# Try installed package first, fall back to source tree headers
set(ROCPROFILER_SDK_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../rocprofiler-sdk"
    CACHE PATH "Path to rocprofiler-sdk source tree (fallback for headers)")

find_package(rocprofiler-sdk QUIET)

if(NOT rocprofiler-sdk_FOUND)
    message(STATUS "rocprofiler-sdk package not found — using source tree headers from:")
    message(STATUS "  ${ROCPROFILER_SDK_SOURCE_DIR}/source/include")

    # Generate version.h from template (normally done by the SDK build system)
    set(PROJECT_VERSION_MAJOR 0)
    set(PROJECT_VERSION_MINOR 7)
    set(PROJECT_VERSION_PATCH 0)
    set(FULL_VERSION_STRING "0.7.0")
    set(ROCPROFILER_SDK_GIT_DESCRIBE "dev")
    set(ROCPROFILER_SDK_GIT_REVISION "unknown")
    configure_file(
        ${ROCPROFILER_SDK_SOURCE_DIR}/source/include/rocprofiler-sdk/version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/rocprofiler-sdk/version.h
        @ONLY
    )

    # Additional source-tree dependencies for the SDK headers
    set(ROCR_LIBHSAKMT_INCLUDE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../rocr-runtime/libhsakmt/include"
        CACHE PATH "Path to libhsakmt include dir (for hsakmt/hsakmttypes.h)")

    # Create an interface target that mimics the installed package
    add_library(rocprofiler-sdk-headers INTERFACE)
    target_include_directories(rocprofiler-sdk-headers INTERFACE
        ${ROCPROFILER_SDK_SOURCE_DIR}/source/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${ROCR_LIBHSAKMT_INCLUDE_DIR}
    )
    add_library(rocprofiler-sdk::rocprofiler-sdk ALIAS rocprofiler-sdk-headers)

    # We also need to link against the installed shared library if available
    find_library(ROCPROFILER_SDK_LIB
        NAMES rocprofiler-sdk64
        HINTS ${ROCM_PATH}/lib
        PATHS ${ROCM_PATH}/lib)

    if(ROCPROFILER_SDK_LIB)
        message(STATUS "Found rocprofiler-sdk library: ${ROCPROFILER_SDK_LIB}")
        target_link_libraries(rocprofiler-sdk-headers INTERFACE ${ROCPROFILER_SDK_LIB})
    else()
        message(WARNING "rocprofiler-sdk shared library not found — "
                        "tool will compile but may fail to link. "
                        "Install rocprofiler-sdk or build from source.")
    endif()
else()
    message(STATUS "Found rocprofiler-sdk package")
endif()

# ---------------------------------------------------------------------------
# Find HSA runtime headers (from rocr-runtime source tree or installed ROCm)
# ---------------------------------------------------------------------------
# The HSA API table structures (HsaApiTable, CoreApiTable, AmdExtTable) are
# defined in hsa_api_trace.h from the rocr-runtime source tree. This header
# is also installed with the hsa-runtime64 package.
#
# Source code uses flat includes (e.g., #include <hsa.h> not #include <hsa/hsa.h>)
# so the include directory must point directly to the directory containing hsa.h.
#
# Search order:
#   1. The rocm-systems monorepo (adjacent to this project) — source tree
#   2. Installed ROCm (/opt/rocm/include/hsa/) — installed layout
#   3. User-specified ROCR_RUNTIME_SOURCE_DIR

set(ROCR_RUNTIME_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../rocr-runtime/runtime/hsa-runtime"
    CACHE PATH "Path to rocr-runtime source tree (for hsa_api_trace.h)")

set(HSA_INCLUDE_DIRS "")

# Look for hsa.h — this tells us which directory layout we have
# Source tree: rocr-runtime/runtime/hsa-runtime/inc/hsa.h (flat)
# Installed:   /opt/rocm/include/hsa/hsa.h (under hsa/ subdir)
find_path(HSA_FLAT_INCLUDE_DIR
    NAMES hsa.h
    HINTS ${ROCR_RUNTIME_SOURCE_DIR}/inc
          ${ROCM_PATH}/include/hsa
    PATHS ${ROCR_RUNTIME_SOURCE_DIR}/inc
          ${ROCM_PATH}/include/hsa
    NO_DEFAULT_PATH)

if(HSA_FLAT_INCLUDE_DIR)
    list(APPEND HSA_INCLUDE_DIRS "${HSA_FLAT_INCLUDE_DIR}")
    message(STATUS "Found HSA headers (hsa.h) in: ${HSA_FLAT_INCLUDE_DIR}")
else()
    message(WARNING "HSA headers (hsa.h) not found — build may fail. "
                    "Set ROCR_RUNTIME_SOURCE_DIR to the rocr-runtime source tree, "
                    "or ensure hsa-runtime64-dev is installed.")
endif()

# Also check that hsa_api_trace.h is in the same directory
# (it defines HsaApiTable, CoreApiTable, AmdExtTable)
if(HSA_FLAT_INCLUDE_DIR AND EXISTS "${HSA_FLAT_INCLUDE_DIR}/hsa_api_trace.h")
    message(STATUS "Found hsa_api_trace.h in: ${HSA_FLAT_INCLUDE_DIR}")
else()
    message(WARNING "hsa_api_trace.h not found alongside hsa.h — "
                    "HsaApiTable struct won't be available.")
endif()

# ---------------------------------------------------------------------------
# HSA runtime library
# ---------------------------------------------------------------------------
find_library(HSA_RUNTIME_LIB
    NAMES hsa-runtime64
    HINTS ${ROCM_PATH}/lib
    PATHS ${ROCM_PATH}/lib)

# ---------------------------------------------------------------------------
# Tool shared library: libaql_dump_tool.so
# ---------------------------------------------------------------------------
add_library(aql_dump_tool SHARED
    src/aql_dump_tool.cpp
    src/aql_dump_tool.h
)

target_include_directories(aql_dump_tool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${HSA_INCLUDE_DIRS}
)

# hsa_api_trace.h uses AMD_INTERNAL_BUILD to select between flat includes
# (e.g. "hsa_ext_image.h") and prefixed includes (e.g. "inc/hsa_ext_image.h").
# Since our include path points directly at the directory containing hsa.h
# (flat layout), we always need AMD_INTERNAL_BUILD.
target_compile_definitions(aql_dump_tool PRIVATE AMD_INTERNAL_BUILD)

target_link_libraries(aql_dump_tool PRIVATE
    rocprofiler-sdk::rocprofiler-sdk
)

if(HSA_RUNTIME_LIB)
    target_link_libraries(aql_dump_tool PRIVATE ${HSA_RUNTIME_LIB})
endif()

# Ensure the rocprofiler_configure symbol is exported
set_target_properties(aql_dump_tool PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# ---------------------------------------------------------------------------
# Test application: aql_test_app (requires HIP compiler)
# ---------------------------------------------------------------------------
if(HIP_AVAILABLE)
    set_source_files_properties(test/aql_test_app.cpp PROPERTIES LANGUAGE HIP)

    add_executable(aql_test_app test/aql_test_app.cpp)

    target_include_directories(aql_test_app PRIVATE
        ${HSA_INCLUDE_DIRS}
    )

    # Convenience test target
    add_custom_target(run_test
        COMMAND ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:aql_dump_tool>"
            "$<TARGET_FILE:aql_test_app>"
        DEPENDS aql_dump_tool aql_test_app
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running aql_test_app with aql_dump_tool preloaded"
    )

    # -----------------------------------------------------------------------
    # Additional test apps from rocprofiler-sdk test suite
    # -----------------------------------------------------------------------
    set(ROCPROFILER_SDK_TESTS_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../rocprofiler-sdk/tests")

    # 1. compute_comm_overlap — 8 streams, async memcpy, kernel, hipEvent timing
    set_source_files_properties(
        ${ROCPROFILER_SDK_TESTS_DIR}/bin/hip-streams/compute_comm_overlap.cpp
        PROPERTIES LANGUAGE HIP)
    add_executable(compute_comm_overlap
        ${ROCPROFILER_SDK_TESTS_DIR}/bin/hip-streams/compute_comm_overlap.cpp)
    target_include_directories(compute_comm_overlap PRIVATE ${HSA_INCLUDE_DIRS})

    # 2. multistream_app — 100 streams × 100 iterations kernel launches
    set_source_files_properties(
        ${ROCPROFILER_SDK_TESTS_DIR}/bin/multistream/multistream_app.cpp
        PROPERTIES LANGUAGE HIP)
    add_executable(multistream_app
        ${ROCPROFILER_SDK_TESTS_DIR}/bin/multistream/multistream_app.cpp)
    target_include_directories(multistream_app PRIVATE ${HSA_INCLUDE_DIRS})

    # 3. vector-ops — 4 different kernels across multiple streams
    set_source_files_properties(
        ${ROCPROFILER_SDK_TESTS_DIR}/bin/vector-operations/vector-ops.cpp
        PROPERTIES LANGUAGE HIP)
    add_executable(vector_ops
        ${ROCPROFILER_SDK_TESTS_DIR}/bin/vector-operations/vector-ops.cpp)
    target_include_directories(vector_ops PRIVATE
        ${HSA_INCLUDE_DIRS}
        ${ROCPROFILER_SDK_TESTS_DIR})  # for common/defines.hpp

else()
    message(STATUS "HIP compiler not found — test application (aql_test_app) will NOT be built")
    message(STATUS "  Set CMAKE_HIP_COMPILER or install ROCm HIP SDK to build the test app")
endif()

# Print configuration summary
message(STATUS "========================================")
message(STATUS "AQL Dump Tool Configuration:")
message(STATUS "  ROCM_PATH:               ${ROCM_PATH}")
message(STATUS "  HSA include dirs:        ${HSA_INCLUDE_DIRS}")
message(STATUS "  HSA runtime lib:         ${HSA_RUNTIME_LIB}")
message(STATUS "  rocprofiler-sdk found:   YES")
message(STATUS "========================================")
