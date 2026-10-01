# MemorySanitizer needs every linked library instrumented: ports build with clang and -fsanitize=memory.
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_C_FLAGS "-fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer")
set(VCPKG_CXX_FLAGS "${VCPKG_C_FLAGS}")
set(VCPKG_LINKER_FLAGS "-fsanitize=memory")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
