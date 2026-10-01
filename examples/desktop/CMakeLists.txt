# The render test scenes (tests/render) in an SDL3 window; ANGLE mode needs SHIROKO_PORT_ANGLE with a dynamic ANGLE.
find_package(SDL3 CONFIG REQUIRED)
set(_render ${PROJECT_SOURCE_DIR}/tests/render)
add_executable(shiroko_desktop main.c ${_render}/stage.c ${_render}/scenes.c)
target_include_directories(shiroko_desktop PRIVATE ${_render})
target_link_libraries(shiroko_desktop PRIVATE shiroko::shiroko SDL3::SDL3)
target_compile_definitions(shiroko_desktop PRIVATE SHR_FONT_DIR="${CMAKE_BINARY_DIR}/fonts"
  SHR_UCD_DIR="${SHIROKO_UCD_CACHE}")
target_compile_options(shiroko_desktop PRIVATE ${SHIROKO_WARNINGS})
add_dependencies(shiroko_desktop shiroko_fonts)
# SDL dlopens ANGLE by these file names and must get the images the executable links (found through its rpath).
if(SHIROKO_PORT_ANGLE)
  get_target_property(_type unofficial::angle::libEGL TYPE)
  if(_type STREQUAL "SHARED_LIBRARY")
    target_link_libraries(shiroko_desktop PRIVATE shiroko::shiroko_angle)
    target_compile_definitions(shiroko_desktop PRIVATE SHR_DESKTOP_ANGLE=1
      ANGLE_EGL_LIBRARY="$<TARGET_FILE_NAME:unofficial::angle::libEGL>"
      ANGLE_GLES_LIBRARY="$<TARGET_FILE_NAME:unofficial::angle::libGLESv2>")
  else()
    message(STATUS "shiroko_desktop: ANGLE is a static library here, so the example is software only (preset desktop)")
  endif()
endif()
