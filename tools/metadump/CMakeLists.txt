# Copyright © 2026 CCP ehf.
#
# blue-metadump: writes metadata.json, the language-neutral description of everything the Blue
# C API exposes, through the C API's introspection functions. Plain C11 over the install layout
# of the headers (blue_c/blue.h + blue_c/BlueCAbi.h) only, like tests/capi. A build tool: not
# installed (DotBlue's build/native.sh runs it from the build tree).

add_executable(blue-metadump blue_metadump.c)
set_target_properties(blue-metadump PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    # blue.so's own build RUNPATH finds libCcpCore in the dependency prefix
    BUILD_RPATH "$<TARGET_FILE_DIR:Blue>"
)
target_include_directories(blue-metadump PRIVATE ${BLUE_CAPI_INCLUDE_DIR})
target_compile_options(blue-metadump PRIVATE -Wall -Wextra -pedantic-errors)
# LINK_ONLY: link blue.so without taking blue's C++ include directories and definitions (see
# tests/capi/CMakeLists.txt)
target_link_libraries(blue-metadump PRIVATE $<LINK_ONLY:Blue>)
