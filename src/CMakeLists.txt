include(FetchContent)

set(SNMALLOC_HEADER_ONLY_LIBRARY ON)
set(SNMALLOC_USE_CXX17 ON CACHE BOOL "Build snmalloc in legacy CXX17 mode as long as Verona stays on CXX17" FORCE)

FetchContent_Declare(
  snmalloc
  GIT_REPOSITORY https://github.com/microsoft/snmalloc
  GIT_TAG        b8e28be14b3fd98e27c2fe87c0296570f6d3990e
)

FetchContent_MakeAvailable(snmalloc)

add_subdirectory(rt)