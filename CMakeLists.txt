cmake_minimum_required(VERSION 3.25)

project(LinkStateFabric
  VERSION 1.0.0
  DESCRIPTION "Authoritative dynamic link-condition runtime of the Distributed Fabric Infrastructure stack"
  HOMEPAGE_URL "https://github.com/pngen/Link-State-Fabric"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(LSF_BUILD_TESTS "Build the Link State Fabric test suite" ON)
option(LSF_BUILD_EXAMPLES "Build the Link State Fabric examples" ON)
option(LSF_BUILD_TOOLS "Build the Link State Fabric command line tools" ON)
option(LSF_BUILD_BENCHMARKS "Build the Link State Fabric benchmarks" OFF)
option(LSF_ENABLE_ASAN "Build with AddressSanitizer (MSVC /fsanitize=address)" OFF)
option(LSF_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)
include(GenerateExportHeader)
include(CMakePackageConfigHelpers)
include(CTest)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# First-party warning policy: /W4 /WX on MSVC, -Wall -Wextra -Werror elsewhere.
# No warning is suppressed; the build must be clean at this level.
# ---------------------------------------------------------------------------
function(lsf_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /Zc:__cplusplus
      /Zc:preprocessor
      /Zc:throwingNew
      /EHsc)
    if(LSF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(LSF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(LSF_PUBLIC_HEADERS
  include/linkstate/authority.hpp
  include/linkstate/client.hpp
  include/linkstate/config.hpp
  include/linkstate/digest.hpp
  include/linkstate/engine.hpp
  include/linkstate/evidence.hpp
  include/linkstate/explanation.hpp
  include/linkstate/generation.hpp
  include/linkstate/ids.hpp
  include/linkstate/limits.hpp
  include/linkstate/outcome.hpp
  include/linkstate/persistence.hpp
  include/linkstate/platform.hpp
  include/linkstate/protocol.hpp
  include/linkstate/record.hpp
  include/linkstate/server.hpp
  include/linkstate/snapshot.hpp
  include/linkstate/state.hpp
  include/linkstate/synthetic.hpp
  include/linkstate/version.hpp)

set(LSF_SOURCES
  src/authority.cpp
  src/client.cpp
  src/config.cpp
  src/digest.cpp
  src/engine.cpp
  src/engine_explain.cpp
  src/engine_persistence.cpp
  src/engine_publish.cpp
  src/engine_queries.cpp
  src/engine_resolution.cpp
  src/engine_support.cpp
  src/evidence.cpp
  src/ids.cpp
  src/net.cpp
  src/outcome.cpp
  src/persistence.cpp
  src/platform_windows.cpp
  src/protocol.cpp
  src/record.cpp
  src/server.cpp
  src/snapshot.cpp
  src/state.cpp
  src/synthetic.cpp
  src/text.cpp
  src/version.cpp)

add_library(LinkStateFabric ${LSF_SOURCES} ${LSF_PUBLIC_HEADERS})
add_library(SummonSoftwareLabs::LinkStateFabric ALIAS LinkStateFabric)

generate_export_header(LinkStateFabric
  BASE_NAME LSF
  EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/linkstate/export.hpp")

target_include_directories(LinkStateFabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

target_link_libraries(LinkStateFabric PRIVATE Threads::Threads)
lsf_apply_warnings(LinkStateFabric)

target_compile_definitions(LinkStateFabric
  PRIVATE LSF_BUILDING_LIBRARY
  PUBLIC LSF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
         LSF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
         LSF_VERSION_PATCH=${PROJECT_VERSION_PATCH})

if(WIN32)
  target_link_libraries(LinkStateFabric PRIVATE ws2_32 iphlpapi)
endif()

if(LSF_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(LinkStateFabric PUBLIC /fsanitize=address /Zi)
    target_link_options(LinkStateFabric PUBLIC /INCREMENTAL:NO)
  else()
    target_compile_options(LinkStateFabric PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(LinkStateFabric PUBLIC -fsanitize=address)
  endif()
endif()

set_target_properties(LinkStateFabric PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(LSF_BUILD_TOOLS)
  add_executable(lsf_inspect apps/lsf_inspect/main.cpp)
  target_link_libraries(lsf_inspect PRIVATE SummonSoftwareLabs::LinkStateFabric)
  lsf_apply_warnings(lsf_inspect)
  set_target_properties(lsf_inspect PROPERTIES OUTPUT_NAME lsf-inspect)

  add_executable(lsf_coordinator apps/lsf_coordinator/main.cpp)
  target_link_libraries(lsf_coordinator PRIVATE SummonSoftwareLabs::LinkStateFabric)
  lsf_apply_warnings(lsf_coordinator)
  set_target_properties(lsf_coordinator PROPERTIES OUTPUT_NAME lsf-coordinator)

  add_executable(lsf_worker apps/lsf_worker/main.cpp)
  target_link_libraries(lsf_worker PRIVATE SummonSoftwareLabs::LinkStateFabric)
  lsf_apply_warnings(lsf_worker)
  set_target_properties(lsf_worker PROPERTIES OUTPUT_NAME lsf-worker)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(LSF_BUILD_EXAMPLES)
  set(LSF_EXAMPLES
    ex01_publish_up
    ex02_down_transition
    ex03_degraded
    ex04_conflicting_evidence
    ex05_stale_generation
    ex06_stale_worker_boot
    ex07_topology_invalidation
    ex08_publisher_loss
    ex09_persistence_recovery)
  foreach(example IN LISTS LSF_EXAMPLES)
    add_executable(${example} examples/${example}.cpp)
    target_link_libraries(${example} PRIVATE SummonSoftwareLabs::LinkStateFabric)
    lsf_apply_warnings(${example})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(LSF_BUILD_BENCHMARKS)
  add_executable(lsf_benchmarks benchmarks/bench_engine.cpp)
  target_link_libraries(lsf_benchmarks PRIVATE SummonSoftwareLabs::LinkStateFabric)
  lsf_apply_warnings(lsf_benchmarks)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LSF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS LinkStateFabric
  EXPORT LinkStateFabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(LSF_BUILD_TOOLS)
  install(TARGETS lsf_inspect lsf_coordinator lsf_worker
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/linkstate
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/linkstate/export.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linkstate)

install(EXPORT LinkStateFabricTargets
  FILE LinkStateFabricTargets.cmake
  NAMESPACE SummonSoftwareLabs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LinkStateFabric)

configure_package_config_file(
  cmake/LinkStateFabricConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/LinkStateFabricConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LinkStateFabric)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LinkStateFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/LinkStateFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LinkStateFabricConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LinkStateFabric)
