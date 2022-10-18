cmake_minimum_required(VERSION 3.10.0)

project(verona-rt-all CXX)

# ## Clang-format target
include(cmake/clangformat.cmake)
clangformat_targets()

## Check copyright/license target
include(cmake/copyright.cmake)
copyright_targets()

include(FetchContent)

set(SNMALLOC_HEADER_ONLY_LIBRARY ON)
set(SNMALLOC_USE_CXX17 ON CACHE BOOL "Build snmalloc in legacy CXX17 mode as long as Verona stays on CXX17" FORCE)

FetchContent_Declare(
  snmalloc
  GIT_REPOSITORY https://github.com/microsoft/snmalloc
  GIT_TAG        ddc5703cc87e092a2cbdf23d779eec1eb16991a3
)

FetchContent_MakeAvailable(snmalloc)

add_subdirectory(src/rt)