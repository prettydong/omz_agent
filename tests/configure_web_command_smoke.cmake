foreach(required_variable IN ITEMS ZEDA_EXECUTABLE TEST_ROOT)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace")
file(WRITE "${TEST_ROOT}/input.txt" "/configure-web\n/exit\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "OPENCODE_GO_API_KEY=fixture-key"
        "ZED_CONFIGURE_WEB_NO_BROWSER=1"
        "ZED_WORKSPACE=${TEST_ROOT}/workspace"
        "ZED_SESSION_PATH=${TEST_ROOT}/session.jsonl"
        "${ZEDA_EXECUTABLE}"
    INPUT_FILE "${TEST_ROOT}/input.txt"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR
        "zeda /configure-web failed (${result}): ${error}")
endif()
if (NOT output MATCHES "configuration: http://127\\.0\\.0\\.1:[0-9]+/\\?token=")
    message(FATAL_ERROR
        "/configure-web URL is missing from output: ${output}")
endif()
if (NOT output MATCHES "settings are stored in .*\\.zed/config\\.json")
    message(FATAL_ERROR
        "/configure-web restart guidance is missing: ${output}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
