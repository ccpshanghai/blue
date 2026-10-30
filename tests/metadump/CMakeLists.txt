# Copyright © 2026 CCP ehf.
#
# Tests of blue-metadump (tools/metadump) in the Python-free flavour. The dumper is built
# without sanitizers (it is a build tool); the C API it calls is covered under ASan/LSan by
# BlueCApiTest.

# Temporary app directories and dumps go to the build directory
set(BLUE_METADUMP_TEST_ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")

# Two runs give byte-identical files
add_test(NAME BlueMetadump.Deterministic
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_deterministic.sh $<TARGET_FILE:blue-metadump> ${CMAKE_CURRENT_BINARY_DIR}/deterministic
)
# The class count matches blue_class_count() (BLUE_CAPI_TEST_CLASS_COUNT, from tests/capi),
# MemStream has its attributes and the IBlueStream methods, and every method is callable
add_test(NAME BlueMetadump.Content
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_metadata.sh $<TARGET_FILE:blue-metadump> ${CMAKE_CURRENT_BINARY_DIR}/content ${BLUE_CAPI_TEST_CLASS_COUNT}
)
set_tests_properties(BlueMetadump.Deterministic BlueMetadump.Content
    PROPERTIES ENVIRONMENT "${BLUE_METADUMP_TEST_ENVIRONMENT}"
)
