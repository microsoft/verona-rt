cmake_minimum_required(VERSION 3.8)
project(verona-rt C CXX)

macro(subdirlist result curdir)
  file(GLOB children LIST_DIRECTORIES true CONFIGURE_DEPENDS RELATIVE ${curdir} ${curdir}/*)
  set(dirlist "")
  foreach(child ${children})
    if(IS_DIRECTORY ${curdir}/${child})
      list(APPEND dirlist ${child})
    endif()
  endforeach()
  set(${result} ${dirlist})
endmacro()

if(NOT MSVC)
  find_file(EXECINFO_H "execinfo.h")
  if(EXISTS ${EXECINFO_H})
    set(USE_EXECINFO ON)
    add_definitions(-DUSE_EXECINFO)
  endif()
endif()


add_library(verona_rt INTERFACE)
# ASAN must be added first, or it gets upset.
if(SANITIZER)
  if(MSVC)
    message(FATAL_ERROR "MSVC does not support sanitizers")
  endif()
  message(STATUS "Run-time sanitizer=${SANITIZER}")
  target_compile_definitions(verona_rt INTERFACE -DSNMALLOC_PASS_THROUGH)
  target_compile_options(verona_rt INTERFACE -g -fsanitize=${SANITIZER} -fno-omit-frame-pointer)
  target_link_libraries(verona_rt INTERFACE -fsanitize=${SANITIZER})
endif()

if(USE_SYSTEMATIC_TESTING)
  target_compile_definitions(verona_rt INTERFACE USE_SYSTEMATIC_TESTING)
endif()

if(USE_CRASH_LOGGING)
  target_compile_definitions(verona_rt INTERFACE USE_FLIGHT_RECORDER)
endif()

if(USE_EXECINFO)
  if (${CMAKE_BUILD_TYPE} MATCHES "Debug|RelWithDebInfo")
    target_link_libraries(verona_rt INTERFACE -rdynamic)
  endif()

  find_library(LIBEXECINFO execinfo)
  if(EXISTS ${LIBEXECINFO})
    target_link_libraries(verona_rt INTERFACE execinfo)
  endif()
endif()

if(VERONA_CI_BUILD)
  target_compile_definitions(verona_rt INTERFACE -DCI_BUILD)
endif()

target_link_libraries(verona_rt INTERFACE snmalloc)
target_include_directories(verona_rt INTERFACE .)

if(MSVC)
  target_compile_definitions(verona_rt INTERFACE -D__EXCEPTIONS)
  target_compile_options(verona_rt INTERFACE /permissive-)
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
else()
  find_package(Threads REQUIRED)
  target_compile_options(verona_rt INTERFACE -mcx16)
endif()

if(USE_SCHED_STATS)
  target_compile_definitions(verona_rt INTERFACE -DUSE_SCHED_STATS)
endif()

target_compile_definitions(verona_rt INTERFACE -DSNMALLOC_CHEAP_CHECKS)

set(CMAKE_CXX_STANDARD 17)

warnings_high()
