cmake_minimum_required(VERSION 3.16)

project(openartemis
  VERSION 0.1.0
  DESCRIPTION "C++/SDL2 Artemis visual-novel engine for VintagePomelo"
  LANGUAGES C CXX)

if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  set(OHOS TRUE)
endif()

if(MSVC)
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

option(OA_USE_SDL2 "Build against SDL2 (VintagePomelo / MinGW)" ON)
option(OA_BUILD_TOOLS "Build bundled command-line tools" OFF)
option(OA_ENABLE_TESTS "Build unit tests" OFF)
option(OA_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(OA_HARMONY_LIB "Build HarmonyOS shared library host" OFF)

if(OHOS)
  set(OA_HARMONY_LIB ON CACHE BOOL "" FORCE)
  set(OA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(OA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  # Dynamic-symbol diet (P1 build item): the HarmonyOS host dlopen()s this
  # library and only looks up the C entry points declared with
  # __attribute__((visibility("default"))) in src/app/harmony/oa_harmony.cpp
  # (runner_main / requestShutdown / cleanupSDL / getLastRubyError /
  # get_last_ruby_error). Everything else — the whole C++ engine surface plus
  # vendored lua/physfs — stays internal: fewer dynamic relocations, smaller
  # .so, faster dlopen, and no accidental ABI surface.
  set(CMAKE_C_VISIBILITY_PRESET hidden)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
  # 出厂性能：跨 TU LTO（docs/PERFORMANCE_OPTIMIZATION_PLAN.md §4）。
  # -O 级别由构建脚本 CMAKE_BUILD_TYPE=Release(-O3) 提供；不开 -ffast-math
  # （像素对照基线约束）。
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(OA_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

if(OA_USE_SDL2)
  add_compile_definitions(OA_USE_SDL2=1 SDL_MAIN_HANDLED)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/oa_deps.cmake)

add_subdirectory(src/core)
add_subdirectory(src/app)

if(OA_BUILD_TOOLS AND NOT OHOS)
  add_subdirectory(tools)
endif()
if(OA_ENABLE_TESTS AND NOT OHOS)
  enable_testing()
  add_subdirectory(tests)
endif()
