# Standalone consumer app: the VT engine and its Zig toolchain belong here,
# not to the Shiroko build.
cmake_minimum_required(VERSION 3.21)
if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()
set(VCPKG_MANIFEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
project(shiroko_vt LANGUAGES C)

set(SHIROKO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SHIROKO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(../.. shiroko EXCLUDE_FROM_ALL)
set(GHOSTTY_ZIG_BUILD_FLAGS "-Dvt-features=-all,+render-state,+grid_introspection;-Dsimd=false" CACHE STRING "")
add_subdirectory(../../third_party/ghostty ghostty)

add_executable(shiroko_vt main.c)
set_target_properties(shiroko_vt PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(shiroko_vt PRIVATE shiroko::shiroko ghostty-vt)
