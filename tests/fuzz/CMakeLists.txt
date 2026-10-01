set(SHIROKO_FUZZ_TARGETS fuzz_tilemap fuzz_package fuzz_driver fuzz_compositor)
set(SHIROKO_FUZZ_CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus)

# fuzz_package also starts from reduced real packages for this build's cell size, zstd and stored, in the smallest
# page shape of at least two cells by one line (small enough for libFuzzer's input limit); byte 0 picks the role and
# mode, byte 1 reseals the checksums (or also maps the package at an odd address).
set(_pkgs ${CMAKE_CURRENT_BINARY_DIR}/seed-packages)
set(_seeds ${CMAKE_CURRENT_BINARY_DIR}/seeds/fuzz_package)
math(EXPR _min_w "2 * ${SHIROKO_CELL_WIDTH}")
foreach(_side 512 256 128 64)
  if(_side GREATER_EQUAL _min_w)
    set(_atlas_w ${_side})
  endif()
  if(_side GREATER_EQUAL SHIROKO_CELL_HEIGHT)
    set(_atlas_h ${_side})
  endif()
endforeach()
set(_scalars 0020-007E,00A1-00FF,AC00-AC2F,1F610-1F61F,1100-1112,1161-1175,11A8-11C2,1F600-1F60F,2764,FE0F,1F469,200D,1F4BB,E0B0-E0B3,00E9,0301,2588,6F22,FFFD)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/seed_packages.py @ONLY CONTENT [[
import pathlib, sys
src, out = map(pathlib.Path, sys.argv[1:3])
out.mkdir(parents=True, exist_ok=True)
for method in ("zstd", "stored"):
    for role, name in enumerate(["latin", "cjk-ko", "symbols", "emoji", "nerd"]):
        data = (src / method / f"shiroko-{name}.shrf").read_bytes()
        for mode, flags in ((0, 1), (0, 3), (0x08, 1), (0x10, 1), (0x20, 1), (0x40, 1), (0x80, 1), (0x88, 1)):
            (out / f"shiroko-{name}-{method}-{mode:02x}-{flags}").write_bytes(bytes([role | mode, flags]) + data)
(out / ".stamp").touch()
]])
set(_seed_build ${SHIROKO_FONTPACK} build latin cjk-ko symbols emoji nerd --scalars ${_scalars}
  --cell ${SHIROKO_CELL_WIDTH}x${SHIROKO_CELL_HEIGHT} --page-atlas ${_atlas_w}x${_atlas_h})
add_custom_command(OUTPUT ${_seeds}/.stamp
  COMMAND ${_seed_build} --out ${_pkgs}/zstd
  COMMAND ${_seed_build} --method stored --out ${_pkgs}/stored
  COMMAND ${SHIROKO_PYTHON} ${CMAKE_CURRENT_BINARY_DIR}/seed_packages.py ${_pkgs} ${_seeds}
  DEPENDS ${SHIROKO_FONT_DEPS} ${CMAKE_CURRENT_BINARY_DIR}/seed_packages.py WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  VERBATIM COMMENT "Baking fuzz_package seeds")
set(_seed_root ${CMAKE_CURRENT_BINARY_DIR}/seeds)
add_custom_command(OUTPUT ${_seed_root}/.stamp
  COMMAND ${SHIROKO_PYTHON} ${PROJECT_SOURCE_DIR}/tools/fuzz/make_seeds.py ${_seed_root}
  COMMAND ${CMAKE_COMMAND} -E touch ${_seed_root}/.stamp
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/fuzz/make_seeds.py VERBATIM COMMENT "Writing fuzz seeds")
set(_all "")
if(SHIROKO_BUILD_TESTS AND NOT SHIROKO_FUZZ)
  set(_all ALL)
endif()
add_custom_target(fuzz_seeds ${_all} DEPENDS ${_seed_root}/.stamp ${_seeds}/.stamp)
add_dependencies(fuzz_seeds shiroko_unicode_tables)

foreach(t IN LISTS SHIROKO_FUZZ_TARGETS)
  if(SHIROKO_FUZZ)
    add_executable(${t} ${t}.c)
    target_link_options(${t} PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(${t} ${t}.c replay_main.c)
  endif()
  target_link_libraries(${t} PRIVATE shiroko)
  target_include_directories(${t} SYSTEM PRIVATE ${UTHASH_INCLUDE_DIR})
  target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/cores/shared)
  target_compile_options(${t} PRIVATE ${SHIROKO_WARNINGS})
  if(NOT SHIROKO_FUZZ AND SHIROKO_BUILD_TESTS)
    set(dirs ${_seed_root}/${t})
    if(EXISTS ${SHIROKO_FUZZ_CORPUS}/${t})
      list(APPEND dirs ${SHIROKO_FUZZ_CORPUS}/${t})
    endif()
    add_test(NAME ${t}_corpus COMMAND ${t} ${dirs})
  endif()
endforeach()
get_property(_tests DIRECTORY PROPERTY TESTS)
if(_tests)
  set_tests_properties(${_tests} PROPERTIES TIMEOUT 300)
endif()
