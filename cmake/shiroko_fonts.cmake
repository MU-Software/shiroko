# The Unicode tables, the font packages and the built-in package, generated (for SHIROKO_CELL_WIDTH x
# SHIROKO_CELL_HEIGHT) from the inputs fetched by `make fontpack-fetch`; the build never downloads and never syncs
# the uv environment.
set(_src ${CMAKE_CURRENT_SOURCE_DIR})
set(SHIROKO_FONT_CACHE ${_src}/.cache/fonts CACHE PATH "Fetched font sources (make fontpack-fetch FONT_CACHE=...)")
set(SHIROKO_UCD_CACHE ${_src}/.cache/ucd CACHE PATH "Fetched Unicode data (make fontpack-fetch UCD_CACHE=...)")
find_program(SHIROKO_UV uv REQUIRED)

set(_cell ${SHIROKO_CELL_WIDTH}x${SHIROKO_CELL_HEIGHT})
set(_fetched ${SHIROKO_FONT_CACHE}/.fetched)
# Checked at configure time, not declared as an OUTPUT: `clean` would delete a custom command's output.
# As a configure dependency, removing it later reruns this check.
if(NOT EXISTS ${_fetched})
  message(FATAL_ERROR "Font sources are missing (${_fetched}): run `make fontpack-fetch` in ${_src}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_fetched})
set(_profile_lock ${_src}/fonts/text_profile.lock.json)
file(READ ${_profile_lock} _profile)
string(JSON _count LENGTH "${_profile}" sources)
math(EXPR _last "${_count} - 1")
set(_ucd_inputs "")
foreach(i RANGE ${_last})
  string(JSON _name MEMBER "${_profile}" sources ${i})
  if(NOT EXISTS ${SHIROKO_UCD_CACHE}/${_name})
    message(FATAL_ERROR "Unicode data is missing (${SHIROKO_UCD_CACHE}/${_name}): "
      "run `make fontpack-fetch UCD_CACHE=${SHIROKO_UCD_CACHE}` in ${_src}")
  endif()
  list(APPEND _ucd_inputs ${SHIROKO_UCD_CACHE}/${_name})
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_profile_lock} ${_ucd_inputs})

set(_unicode ${CMAKE_CURRENT_BINARY_DIR}/generated/shr_gen_unicode_tables.c)
set(SHIROKO_PYTHON ${CMAKE_COMMAND} -E env --unset=VIRTUAL_ENV PYTHONDONTWRITEBYTECODE=1
  SHIROKO_FONT_CACHE=${SHIROKO_FONT_CACHE} SHIROKO_UCD_CACHE=${SHIROKO_UCD_CACHE} SHIROKO_FONT_WORK=${CMAKE_BINARY_DIR}/fontwork
  SHIROKO_UNICODE_TABLES=${_unicode}
  ${SHIROKO_UV} run --project ${_src} --frozen --no-sync --offline --no-python-downloads python)
set(SHIROKO_FONTPACK ${SHIROKO_PYTHON} ${_src}/tools/fontpack/fontpack.py)
execute_process(COMMAND ${SHIROKO_PYTHON} -c "import fontTools, freetype, uharfbuzz, PIL, pooch, xxhash, zstandard"
  WORKING_DIRECTORY ${_src} RESULT_VARIABLE _ok OUTPUT_QUIET ERROR_QUIET)
if(NOT _ok EQUAL 0)
  message(FATAL_ERROR "The uv tools environment is missing: run `make fontpack-fetch` in ${_src}")
endif()
# Rewritten only when the size changes, so every generator reruns the bake on a new size.
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shiroko_cell.txt CONTENT "${_cell}\n")
file(GLOB _font_json CONFIGURE_DEPENDS ${_src}/fonts/*.json)

# One target owns the tables so parallel Makefile builds do not generate them twice; consumers depend on it.
add_custom_command(OUTPUT ${_unicode}
  COMMAND ${SHIROKO_PYTHON} ${_src}/tools/unicode/gen_unicode_tables.py --out ${_unicode}
  DEPENDS ${_src}/tools/unicode/gen_unicode_tables.py ${_profile_lock} ${_ucd_inputs} ${_src}/pyproject.toml ${_src}/uv.lock
  WORKING_DIRECTORY ${_src} VERBATIM COMMENT "Generating the Unicode tables")
add_custom_target(shiroko_unicode_tables DEPENDS ${_unicode})
target_sources(shiroko_shared PRIVATE ${_unicode})
add_dependencies(shiroko_shared shiroko_unicode_tables)
file(GLOB_RECURSE _sprite_py CONFIGURE_DEPENDS ${_src}/tools/fontpack/sprite/*.py)
set(SHIROKO_FONT_DEPS ${_fetched} ${CMAKE_CURRENT_BINARY_DIR}/shiroko_cell.txt ${_font_json}
  ${_src}/tools/fontpack/fontpack.py ${_src}/tools/fontpack/nerd_rules.py ${_sprite_py} ${_unicode}
  ${_src}/pyproject.toml ${_src}/uv.lock)

set(_builtin ${CMAKE_CURRENT_BINARY_DIR}/generated/shr_gen_builtin_package.c)
add_custom_command(OUTPUT ${_builtin}
  COMMAND ${SHIROKO_FONTPACK} builtin --cell ${_cell} --out ${_builtin}
  DEPENDS ${SHIROKO_FONT_DEPS} WORKING_DIRECTORY ${_src} VERBATIM
  COMMENT "Baking the built-in font package (${_cell})")
target_sources(shiroko_pl_res_bitmap_font PRIVATE ${_builtin})
add_dependencies(shiroko_pl_res_bitmap_font shiroko_unicode_tables)

set(_fonts ${CMAKE_BINARY_DIR}/fonts)
file(READ ${_src}/fonts/fontpack.config.json _config)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_src}/fonts/fontpack.config.json)
string(JSON _count LENGTH "${_config}" default)
math(EXPR _last "${_count} - 1")
set(_outputs ${_fonts}/NOTICE ${_fonts}/inventory.json ${_fonts}/coverage.json)
foreach(i RANGE ${_last})
  string(JSON _name GET "${_config}" default ${i})
  list(APPEND _outputs ${_fonts}/shiroko-${_name}.shrf)
endforeach()
set(_method $<IF:$<BOOL:${SHIROKO_ZSTD}>,zstd,stored>)
add_custom_command(OUTPUT ${_outputs}
  COMMAND ${SHIROKO_FONTPACK} build --cell ${_cell} --out ${_fonts} --method ${_method}
  DEPENDS ${SHIROKO_FONT_DEPS} WORKING_DIRECTORY ${_src} VERBATIM
  COMMENT "Baking font packages (${_cell}) into ${_fonts}")
# Runtime data, not needed to link the library: built by default only in a top-level build.
if(PROJECT_IS_TOP_LEVEL)
  set(_all ALL)
endif()
add_custom_target(shiroko_fonts ${_all} DEPENDS ${_outputs})
add_dependencies(shiroko_fonts shiroko_unicode_tables)
install(DIRECTORY ${_fonts}/ DESTINATION ${CMAKE_INSTALL_DATADIR}/shiroko/fonts COMPONENT fonts OPTIONAL
  PATTERN "*.png" EXCLUDE)
