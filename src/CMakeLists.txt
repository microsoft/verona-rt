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
  GIT_TAG        b8e28be14b3fd98e27c2fe87c0296570f6d3990e
)

# Don't add targets for snmalloc tests and clangformat.
set(SNMALLOC_BUILD_TESTING OFF)
# Don't add any of the targets to `all`.
FetchContent_MakeAvailable_ExcludeFromAll(snmalloc)

add_subdirectory(rt)