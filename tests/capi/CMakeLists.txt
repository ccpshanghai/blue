# Copyright © 2026 CCP ehf.
#
# Tests of the Blue C API (include/blue_c/blue.h) in the Python-free flavour.

option(BLUE_CAPI_TEST_ASAN "Build BlueCApiTest with AddressSanitizer and LeakSanitizer" ON)

find_package(Threads REQUIRED)

# Plain C11 against the install layout of the headers (blue_c/blue.h + blue_c/BlueCAbi.h) only
add_executable(BlueCApiTest blue_capi_test.c)
set_target_properties(BlueCApiTest PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    # blue.so's own build RUNPATH finds libCcpCore in the dependency prefix
    BUILD_RPATH "$<TARGET_FILE_DIR:Blue>"
)
target_include_directories(BlueCApiTest PRIVATE ${BLUE_CAPI_INCLUDE_DIR})
target_compile_options(BlueCApiTest PRIVATE -Wall -Wextra -pedantic-errors)
# LINK_ONLY: link blue.so without taking blue's C++ include directories and definitions.
# (A bare $<TARGET_LINKER_FILE:Blue> becomes -L<dir> -lblue when blue.so does not exist yet at
# configure time, which never matches a file named blue.so.)
target_link_libraries(BlueCApiTest PRIVATE $<LINK_ONLY:Blue> Threads::Threads)
# BlueRegistration::GetClassRegs() inside blue.so of this flavour on Linux, 2026-10-29 (blue's
# classes plus blueexposure's BlueClasses, Copier, List and Dict). Update it when a class is
# added or removed. tests/metadump checks metadata.json against it too.
set(BLUE_CAPI_TEST_CLASS_COUNT 48)
set(BLUE_CAPI_TEST_CLASS_COUNT ${BLUE_CAPI_TEST_CLASS_COUNT} PARENT_SCOPE)
target_compile_definitions(BlueCApiTest PRIVATE
    BLUE_CAPI_TEST_CLASS_COUNT=${BLUE_CAPI_TEST_CLASS_COUNT}
    # Of those, the ones blue_class_kind reports as abstract (null factory) and as singletons
    # (2026-10-30); the rest are creatable
    BLUE_CAPI_TEST_ABSTRACT_COUNT=7
    BLUE_CAPI_TEST_SINGLETON_COUNT=6
)
if(BLUE_CAPI_TEST_ASAN)
    target_compile_options(BlueCApiTest PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(BlueCApiTest PRIVATE -fsanitize=address)
endif()

# alloc_dealloc_mismatch: carbon-core's CcpThread.cpp ThreadProcHelper deletes with operator
# delete what CCPMalloc allocated, on every blue worker thread start
set(BLUE_CAPI_TEST_ENVIRONMENT
    "ASAN_OPTIONS=alloc_dealloc_mismatch=0:detect_leaks=1"
    "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
)

# Blue starts once per process, so each lifecycle path is its own process
add_test(NAME BlueCApiTest COMMAND BlueCApiTest)
add_test(NAME BlueCApiTest.InitFailure COMMAND BlueCApiTest init-failure)
add_test(NAME BlueCApiTest.NoShutdown COMMAND BlueCApiTest no-shutdown)
add_test(NAME BlueCApiTest.ResourceWaitShutdown COMMAND BlueCApiTest resource-wait-shutdown)
set_tests_properties(BlueCApiTest BlueCApiTest.InitFailure BlueCApiTest.NoShutdown BlueCApiTest.ResourceWaitShutdown
    PROPERTIES ENVIRONMENT "${BLUE_CAPI_TEST_ENVIRONMENT}"
)

if(BLUE_CAPI_EXPORTS_ONLY)
    if(NOT CMAKE_NM)
        message(FATAL_ERROR "BlueCApiExports needs nm (CMAKE_NM)")
    endif()
    add_test(NAME BlueCApiExports
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_exports.sh ${CMAKE_NM} $<TARGET_FILE:Blue> ${BLUE_CAPI_INCLUDE_DIR}/blue_c/blue.h
    )
endif()
