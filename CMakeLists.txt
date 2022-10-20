cmake_minimum_required(VERSION 3.10.0)

project(verona-rt-all CXX)

# ## Clang-format target
include(cmake/clangformat.cmake)
clangformat_targets()

## Check copyright/license target
include(cmake/copyright.cmake)
copyright_targets()

add_subdirectory(src)

enable_testing()
add_subdirectory(test)

