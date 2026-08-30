foreach(required_variable IN ITEMS ZEDA_EXECUTABLE PLUGIN_ROOT TEST_ROOT)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace")
file(WRITE "${TEST_ROOT}/input.txt" "/deepwiki tui\n/exit\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "OPENCODE_GO_API_KEY=fixture-key"
        "ZED_WORKSPACE=${TEST_ROOT}/workspace"
        "ZED_SESSION_PATH=${TEST_ROOT}/session.jsonl"
        "ZED_PLUGIN_PATH=${PLUGIN_ROOT}"
        "${ZEDA_EXECUTABLE}"
    INPUT_FILE "${TEST_ROOT}/input.txt"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR
        "zeda /deepwiki tui failed (${result}): ${error}")
endif()
if (NOT output MATCHES "interactive document views require a TTY")
    message(FATAL_ERROR
        "/deepwiki tui did not report its TTY requirement: ${output}")
endif()
if (output MATCHES "schema_version")
    message(FATAL_ERROR
        "/deepwiki tui leaked its document protocol to plain output")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
