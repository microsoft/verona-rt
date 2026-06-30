cmake_minimum_required(VERSION 3.10.0)

project(verona-rt-all CXX)

add_subdirectory(src)

# To build with just the header library target define VERONA_RT_ONLY_HEADER_LIBRARY
# in containing Cmake file.
if(NOT DEFINED VERONA_RT_ONLY_HEADER_LIBRARY)
  enable_testing()
  add_subdirectory(test)

  ## Check copyright/license target
  include(cmake/copyright.cmake)
  copyright_targets()
endif()

# Standalone benchmark: THE vs Verona WSQ
add_executable(cilk_wsq test/perf/wsq_compare/cilk_wsq.cc)
target_link_libraries(cilk_wsq verona_rt)
target_include_directories(cilk_wsq PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/rt)
