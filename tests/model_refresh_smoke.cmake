foreach(required_variable IN ITEMS ZEDA_EXECUTABLE FAKE_OPENCODE_EXECUTABLE
                                   TEST_ROOT)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace")
file(WRITE "${TEST_ROOT}/exit.txt" "/model list\n/exit\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "OPENCODE_GO_API_KEY=fixture-key"
        "ZED_WORKSPACE=${TEST_ROOT}/workspace"
        "ZED_SESSION_PATH=${TEST_ROOT}/startup-session.jsonl"
        "ZED_OPENCODE_PATH=${FAKE_OPENCODE_EXECUTABLE}"
        "${ZEDA_EXECUTABLE}"
    INPUT_FILE "${TEST_ROOT}/exit.txt"
    OUTPUT_VARIABLE startup_output
    ERROR_VARIABLE startup_error
    RESULT_VARIABLE startup_result)
if (NOT startup_result EQUAL 0)
    message(FATAL_ERROR
        "zeda startup failed (${startup_result}): ${startup_error}")
endif()
if (startup_output MATCHES "manual-refresh-model")
    message(FATAL_ERROR "zeda discovered models during startup")
endif()
if (NOT startup_output MATCHES
    "startup: [0-9]+\\.[0-9][0-9][0-9] ms = ")
    message(FATAL_ERROR
        "startup timing is missing from the welcome output: ${startup_output}")
endif()

file(WRITE "${TEST_ROOT}/refresh.txt" "/model refresh\n/model list\n/exit\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "OPENCODE_GO_API_KEY=fixture-key"
        "ZED_WORKSPACE=${TEST_ROOT}/workspace"
        "ZED_SESSION_PATH=${TEST_ROOT}/refresh-session.jsonl"
        "ZED_OPENCODE_PATH=${FAKE_OPENCODE_EXECUTABLE}"
        "${ZEDA_EXECUTABLE}"
    INPUT_FILE "${TEST_ROOT}/refresh.txt"
    OUTPUT_VARIABLE refresh_output
    ERROR_VARIABLE refresh_error
    RESULT_VARIABLE refresh_result)
if (NOT refresh_result EQUAL 0)
    message(FATAL_ERROR
        "zeda model refresh failed (${refresh_result}): ${refresh_error}")
endif()
if (NOT refresh_output MATCHES "refreshed OpenCode Go models: 1" OR
    NOT refresh_output MATCHES "manual-refresh-model")
    message(FATAL_ERROR
        "refreshed model catalog is missing from output: ${refresh_output}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
