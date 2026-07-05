cmake_minimum_required(VERSION 3.28)
project(indexed VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Linux-only (§3 of docs/../indexed-plan.md)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "indexed targets Linux only.")
endif()

# ---------- Third-party via FetchContent (docs/adr/0004) ----------
# NOTE: strict warnings are applied only to our own targets (cmake/CompilerWarnings.cmake),
# after FetchContent, so third-party code isn't held to -Werror.
include(FetchContent)

# Several fetched third-party CMakeLists.txt files declare a cmake_minimum_required()
# below what CMake 4.x still accepts (e.g. utf8proc). Allow those old policies to still
# configure rather than hard-erroring; this only affects FetchContent'd sources, not ours.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    absl
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG        20240722.0
)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(absl)

FetchContent_Declare(
    re2
    GIT_REPOSITORY https://github.com/google/re2.git
    GIT_TAG        2024-03-01
    # re2's install(EXPORT) fails when absl targets aren't exported; strip it.
    PATCH_COMMAND ${CMAKE_COMMAND} -P "${CMAKE_SOURCE_DIR}/cmake/patch_re2.cmake"
)
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(re2)

FetchContent_Declare(
    utf8proc
    GIT_REPOSITORY https://github.com/JuliaStrings/utf8proc.git
    GIT_TAG        v2.9.0
)
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(utf8proc)

# ---------- Strict warnings for our own targets ----------
include(cmake/CompilerWarnings.cmake)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# ---------- Subdirectories ----------
enable_testing()
add_subdirectory(src/core)
add_subdirectory(src/ui)
add_subdirectory(src/helper)
add_subdirectory(tests)

# ---------- Optional documentation ----------
option(BUILD_DOCS "Build Doxygen documentation" OFF)
if(BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        doxygen_add_docs(docs ${CMAKE_SOURCE_DIR}/src COMMENT "Generate API docs")
    endif()
endif()
