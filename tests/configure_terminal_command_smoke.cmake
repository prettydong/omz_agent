foreach(required_variable IN ITEMS ZEDA_EXECUTABLE TEST_ROOT)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace")
file(WRITE "${TEST_ROOT}/input.txt"
    "/new terminal-config\n"
    "/configure model glm-5.1\n"
    "/configure model\n"
    "/configure reasoning high\n"
    "/configure model missing-default-model\n"
    "/model\n"
    "/configure agent set max-turns 17\n"
    "/configure agent set tools read,grep\n"
    "/configure subagent set explorer enabled off\n"
    "/configure context set trigger 100000\n"
    "/configure context set max-tokens 200000\n"
    "/configure context set max-tokens 1\n"
    "/configure agent set temperature nan\n"
    "/configure agent set reasoning thinking\n"
    "/configure skill list\n"
    "/exit\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "OPENCODE_GO_API_KEY=fixture-key"
        "ZED_WORKSPACE=${TEST_ROOT}/workspace"
        "ZED_SESSION_PATH=${TEST_ROOT}/session.jsonl"
        "${ZEDA_EXECUTABLE}"
    INPUT_FILE "${TEST_ROOT}/input.txt"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR
        "zeda /configure failed (${result}): ${error}\n${output}")
endif()
if (NOT output MATCHES "configuration saved to .*\\.zed/config\\.json")
    message(FATAL_ERROR "/configure save response is missing: ${output}")
endif()
if (NOT output MATCHES "default model: glm-5.1")
    message(FATAL_ERROR "/configure model getter is missing: ${output}")
endif()
if (NOT output MATCHES "reasoning reset to auto")
    message(FATAL_ERROR "/configure model did not reset incompatible reasoning: ${output}")
endif()
if (NOT output MATCHES "reasoning effort is not supported by the active Agent model")
    message(FATAL_ERROR "/configure reasoning did not validate the default model: ${output}")
endif()
if (NOT output MATCHES "model not found; use /model list")
    message(FATAL_ERROR "/configure model did not reject an unknown model: ${output}")
endif()
if (NOT output MATCHES "main: muse-spark-1.2-contributor")
    message(FATAL_ERROR "/configure model changed the current session model: ${output}")
endif()
if (NOT output MATCHES "no managed workspace skills")
    message(FATAL_ERROR "/configure skill listing is missing: ${output}")
endif()
if (NOT output MATCHES "temperature must be finite")
    message(FATAL_ERROR "/configure did not reject a non-finite temperature: ${output}")
endif()
if (NOT output MATCHES "reasoning effort is not supported")
    message(FATAL_ERROR "/configure did not reject unsupported reasoning: ${output}")
endif()
if (output MATCHES "fixture-key")
    message(FATAL_ERROR "/configure exposed the API credential")
endif()

file(READ "${TEST_ROOT}/workspace/.zed/config.json" config)
if (NOT config MATCHES "\\\"max_turns\\\": 17")
    message(FATAL_ERROR "Agent max turns were not persisted: ${config}")
endif()
if (NOT config MATCHES "\\\"model\\\": \"glm-5.1\"")
    message(FATAL_ERROR "Default model was not persisted: ${config}")
endif()
if (NOT config MATCHES "\\\"reasoning\\\": \"auto\"")
    message(FATAL_ERROR "Compatible default reasoning was not persisted: ${config}")
endif()
if (config MATCHES "missing-default-model")
    message(FATAL_ERROR "Unknown default model was written: ${config}")
endif()
if (NOT config MATCHES "\\\"max_tokens\\\": 200000")
    message(FATAL_ERROR "Context max tokens were not persisted: ${config}")
endif()
if (NOT config MATCHES "\\\"compaction_trigger_tokens\\\": 100000")
    message(FATAL_ERROR "Context trigger was not persisted: ${config}")
endif()
if (NOT config MATCHES "\\\"enabled\\\": false")
    message(FATAL_ERROR "Explorer enabled state was not persisted: ${config}")
endif()

file(READ "${TEST_ROOT}/workspace/.zed/agent_management.json" management)
if (NOT management MATCHES "\\\"read\\\"" OR NOT management MATCHES "\\\"grep\\\"")
    message(FATAL_ERROR "Agent tool permissions were not persisted: ${management}")
endif()

file(WRITE "${TEST_ROOT}/restart.txt" "/configure model\n/configure reasoning\n/exit\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "OPENCODE_GO_API_KEY=fixture-key"
        "ZED_WORKSPACE=${TEST_ROOT}/workspace"
        "ZED_SESSION_PATH=${TEST_ROOT}/restart-session.jsonl"
        "${ZEDA_EXECUTABLE}"
    INPUT_FILE "${TEST_ROOT}/restart.txt"
    OUTPUT_VARIABLE restart_output
    ERROR_VARIABLE restart_error
    RESULT_VARIABLE restart_result)
if (NOT restart_result EQUAL 0)
    message(FATAL_ERROR
        "zeda restart after /configure failed (${restart_result}): ${restart_error}\n${restart_output}")
endif()
if (NOT restart_output MATCHES "model: glm-5\\.1.*reasoning: auto")
    message(FATAL_ERROR "Restart banner did not use persisted default model: ${restart_output}")
endif()
if (NOT restart_output MATCHES "default model: glm-5.1" OR
    NOT restart_output MATCHES "default reasoning: auto")
    message(FATAL_ERROR "Restart did not reload saved /configure defaults: ${restart_output}")
endif()
if (restart_output MATCHES "fixture-key")
    message(FATAL_ERROR "Restart exposed the API credential")
endif()

file(GLOB created_sessions "${TEST_ROOT}/workspace/.zed/sessions/*.jsonl")
list(LENGTH created_sessions created_session_count)
if (NOT created_session_count EQUAL 1)
    message(FATAL_ERROR "/new did not create exactly one Session v2 file")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
