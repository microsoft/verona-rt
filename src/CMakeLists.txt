include(FetchContent)

set(SNMALLOC_HEADER_ONLY_LIBRARY ON)
set(SNMALLOC_USE_CXX17 ON CACHE BOOL "Build snmalloc in legacy CXX17 mode as long as Verona stays on CXX17" FORCE)

FetchContent_Declare(
  snmalloc
  GIT_REPOSITORY https://github.com/microsoft/snmalloc
  GIT_TAG        d8f174c717500a834229da5f1f6bfe888442e81a
)

FetchContent_MakeAvailable(snmalloc)

add_subdirectory(rt)