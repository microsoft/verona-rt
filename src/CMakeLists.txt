include(FetchContent)

# Used to provide
#  FetchContent_MakeAvailable_ExcludeFromAll
FetchContent_Declare(
  cmake_utils
  GIT_REPOSITORY https://github.com/mjp41/cmake_utils
  GIT_TAG 28490f85c91053dc501f96a7f8224e8191156ec9
)

FetchContent_MakeAvailable(cmake_utils)

set(SNMALLOC_USE_CXX17 ON CACHE BOOL "Build snmalloc in legacy CXX17 mode as long as Verona stays on CXX17" FORCE)

FetchContent_Declare(
  snmalloc
  GIT_REPOSITORY https://github.com/microsoft/snmalloc
  GIT_TAG        3fca4c8732b14614163f06330cb0e3148a017d0c
)

# Don't add targets for snmalloc tests and clangformat.
set(SNMALLOC_BUILD_TESTING OFF)
# Don't add any of the targets to `all`.
FetchContent_MakeAvailable_ExcludeFromAll(snmalloc)

clangformat_targets(
  INCLUDE_PATTERNS *.h *.cc
)
add_subdirectory(rt)