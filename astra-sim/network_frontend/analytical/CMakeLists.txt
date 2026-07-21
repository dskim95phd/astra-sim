# CMake Requirement
cmake_minimum_required(VERSION 3.15)

# C++ requirement
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the build type to Release if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# Setup project
project(AstraSim_Analytical)

include(CTest)

# Protobuf is discovered in the AstraSim library subdirectory as well, but
# imported targets created there are not visible to this sibling directory.
if (DEFINED ENV{PROTOBUF_FROM_SOURCE} AND
    "$ENV{PROTOBUF_FROM_SOURCE}" STREQUAL "True")
    find_package(protobuf CONFIG REQUIRED)
else ()
    find_package(Protobuf REQUIRED)
endif ()

get_filename_component(
    WORKLOAD_PROTO_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../proto"
    ABSOLUTE
)
set(WORKLOAD_PROTO "${WORKLOAD_PROTO_DIR}/llmservingsim_workload.proto")
set(WORKLOAD_PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(WORKLOAD_PROTO_SOURCE
        "${WORKLOAD_PROTO_GENERATED_DIR}/llmservingsim_workload.pb.cc")
set(WORKLOAD_PROTO_HEADER
        "${WORKLOAD_PROTO_GENERATED_DIR}/llmservingsim_workload.pb.h")

find_program(
    WORKLOAD_PROTOC
    NAMES protoc
    HINTS ${Protobuf_PROTOC_EXECUTABLE}
    REQUIRED
)

add_custom_command(
    OUTPUT ${WORKLOAD_PROTO_SOURCE} ${WORKLOAD_PROTO_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WORKLOAD_PROTO_GENERATED_DIR}
    COMMAND ${WORKLOAD_PROTOC}
            --cpp_out=${WORKLOAD_PROTO_GENERATED_DIR}
            --proto_path=${WORKLOAD_PROTO_DIR}
            ${WORKLOAD_PROTO}
    DEPENDS ${WORKLOAD_PROTO}
    VERBATIM
)

add_library(WorkloadIpcProto STATIC ${WORKLOAD_PROTO_SOURCE})
target_include_directories(
    WorkloadIpcProto PUBLIC ${WORKLOAD_PROTO_GENERATED_DIR})
if (TARGET protobuf::libprotobuf)
    target_link_libraries(WorkloadIpcProto PUBLIC protobuf::libprotobuf)
else ()
    target_link_libraries(WorkloadIpcProto PUBLIC ${Protobuf_LIBRARIES})
endif ()

if (BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(
        WorkloadIpcAtomicityTest
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/WorkloadIpcAtomicityTest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/common/TemplateRegistry.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/common/WorkloadIpcServer.cc
    )
    add_dependencies(WorkloadIpcAtomicityTest WorkloadIpcProto)
    target_link_libraries(
        WorkloadIpcAtomicityTest
        PRIVATE AstraSim WorkloadIpcProto Threads::Threads
    )
    target_include_directories(
        WorkloadIpcAtomicityTest
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/helper
    )
    add_test(NAME WorkloadIpcAtomicity COMMAND WorkloadIpcAtomicityTest)
endif ()

# Compilation target
set(BUILDTARGET "all" CACHE STRING "Compilation target ([all]/congestion_unaware/congestion_aware)")

# Include src files to compile
file(GLOB srcs_common
        ${CMAKE_CURRENT_SOURCE_DIR}/common/*.cc
)

file(GLOB srcs_congestion_unaware
        ${CMAKE_CURRENT_SOURCE_DIR}/congestion_unaware/*.cc
)

file(GLOB srcs_congestion_aware
        ${CMAKE_CURRENT_SOURCE_DIR}/congestion_aware/*.cc
)

# Compile Congestion Unaware Backend
if (BUILDTARGET STREQUAL "all" OR BUILDTARGET STREQUAL "congestion_unaware")
    add_executable(AstraSim_Analytical_Congestion_Unaware ${srcs_congestion_unaware} ${srcs_common})
    add_dependencies(AstraSim_Analytical_Congestion_Unaware WorkloadIpcProto)
    target_sources(AstraSim_Analytical_Congestion_Unaware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/congestion_unaware/main.cc)

    # Link libraries
    target_link_libraries(AstraSim_Analytical_Congestion_Unaware LINK_PRIVATE AstraSim)
    target_link_libraries(AstraSim_Analytical_Congestion_Unaware LINK_PRIVATE Analytical_Congestion_Unaware)
    target_link_libraries(AstraSim_Analytical_Congestion_Unaware LINK_PRIVATE WorkloadIpcProto)

    # Include directories
    target_include_directories(AstraSim_Analytical_Congestion_Unaware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/)
    target_include_directories(AstraSim_Analytical_Congestion_Unaware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/)
    target_include_directories(AstraSim_Analytical_Congestion_Unaware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/helper)

    # Properties
    # TODO: Switch to OFF after binary_function deprecation has been resolved
    set_target_properties(AstraSim_Analytical_Congestion_Unaware PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    set_target_properties(AstraSim_Analytical_Congestion_Unaware
            PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../bin/
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
    )
endif ()

# Compile Congestion Aware Backend
if (BUILDTARGET STREQUAL "all" OR BUILDTARGET STREQUAL "congestion_aware")
    add_executable(AstraSim_Analytical_Congestion_Aware ${srcs_congestion_aware} ${srcs_common})
    add_dependencies(AstraSim_Analytical_Congestion_Aware WorkloadIpcProto)
    target_sources(AstraSim_Analytical_Congestion_Aware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/congestion_aware/main.cc)

    # Link libraries
    target_link_libraries(AstraSim_Analytical_Congestion_Aware LINK_PRIVATE AstraSim)
    target_link_libraries(AstraSim_Analytical_Congestion_Aware LINK_PRIVATE Analytical_Congestion_Aware)
    target_link_libraries(AstraSim_Analytical_Congestion_Aware LINK_PRIVATE WorkloadIpcProto)

    # Include directories
    target_include_directories(AstraSim_Analytical_Congestion_Aware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/)
    target_include_directories(AstraSim_Analytical_Congestion_Aware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/)
    target_include_directories(AstraSim_Analytical_Congestion_Aware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/helper)

    # Properties
    # TODO: Switch to OFF after binary_function deprecation has been resolved
    set_target_properties(AstraSim_Analytical_Congestion_Aware PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    set_target_properties(AstraSim_Analytical_Congestion_Aware
            PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../bin/
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
    )
endif ()
