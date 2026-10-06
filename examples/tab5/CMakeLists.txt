cmake_minimum_required(VERSION 3.22)
set(PROJECT_VER 0.1.0) # git describe cannot see the worktree's repository from the container
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(shiroko_tab5)

# Each package goes to the partition named after it, copied into the build so flash_args stays relative.
foreach(_name latin cjk-ko symbols emoji nerd)
  set(_part shiroko-${_name})
  set(_bin ${CMAKE_BINARY_DIR}/fonts/${_part}.shrf)
  configure_file(${SHIROKO_TAB5_HOST}/fonts/${_part}.shrf ${_bin} COPYONLY)
  file(SIZE ${_bin} _size)
  partition_table_get_partition_info(_room "--partition-name ${_part}" "size")
  if(_size GREATER _room)
    message(FATAL_ERROR "${_part}.shrf (${_size} B) does not fit its partition (${_room} B)")
  endif()
  esptool_py_flash_to_partition(flash ${_part} ${_bin})
endforeach()
