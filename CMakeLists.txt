cmake_minimum_required(VERSION 3.20)

project(zeda VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui.git
    GIT_TAG v7.0.3
)
FetchContent_MakeAvailable(ftxui)

option(ZEDA_BUILD_DEEPWIKI_PLUGIN "Build the bundled DeepWiki plugin" ON)

set(ZEDA_DEFAULT_SYSTEM_PROMPT_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/zed_system_propmt.md)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${ZEDA_DEFAULT_SYSTEM_PROMPT_FILE})
file(READ ${ZEDA_DEFAULT_SYSTEM_PROMPT_FILE} ZEDA_DEFAULT_SYSTEM_PROMPT)
string(FIND "${ZEDA_DEFAULT_SYSTEM_PROMPT}" ")ZEDA_PROMPT\""
    ZEDA_DEFAULT_SYSTEM_PROMPT_DELIMITER)
if (NOT ZEDA_DEFAULT_SYSTEM_PROMPT_DELIMITER EQUAL -1)
    message(FATAL_ERROR
        "The default system prompt contains the reserved raw-string delimiter")
endif()
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/zed/core)
configure_file(
    cmake/default_system_prompt.hpp.in
    generated/zed/core/default_system_prompt.hpp
    @ONLY)

if (ZEDA_BUILD_DEEPWIKI_PLUGIN)
    find_package(SQLite3 REQUIRED)
    set(HTTPLIB_INSTALL OFF CACHE BOOL "Do not install zeda's private cpp-httplib dependency" FORCE)
    FetchContent_Declare(
        cpp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.51.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(cpp_httplib)

    FetchContent_Declare(
        marked_web
        URL https://registry.npmjs.org/marked/-/marked-18.0.11.tgz
        URL_HASH SHA1=0f50ce5cdf25b2c07f67d3787220b64a5845cce1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_Declare(
        dompurify_web
        URL https://registry.npmjs.org/dompurify/-/dompurify-3.4.14.tgz
        URL_HASH SHA1=a789edb2c7bcdb69a93713a34edb7e0a5245d8c9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_Declare(
        mermaid_web
        URL https://registry.npmjs.org/mermaid/-/mermaid-11.17.2.tgz
        URL_HASH SHA1=e3caf3717582c0e44e5d04cff043e025caec7cba
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(marked_web dompurify_web mermaid_web)
endif()

add_library(zed_core
    src/app/config.cpp
    src/core/agent_loop.cpp
    src/core/context.cpp
    src/core/model_context_controller.cpp
    src/core/session_store.cpp
    src/core/tool_registry.cpp
    src/core/utf8.cpp
    src/lsp/clangd_client.cpp
    src/plugins/plugin_manager.cpp
    src/providers/opencode_go_catalog.cpp
    src/providers/opencode_go_model.cpp
    src/session/jsonl_session_store.cpp
    src/session/session_catalog.cpp
    src/extensions/extension_registry.cpp
    src/extensions/quick_bash_input.cpp
    src/skills/skill_registry.cpp
    src/subagents/agent_registry.cpp
    src/subagents/subagent_runner.cpp
    src/subagents/worker.cpp
    src/subagents/worker_protocol.cpp
    src/tools/basic_tools.cpp
    src/tools/clangd_tool.cpp
    src/tools/multi_bash_tool.cpp
    src/tools/subagent_tool.cpp
)

target_include_directories(zed_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(zed_core
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        ${CMAKE_DL_LIBS}
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zed_core PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
elseif (MSVC)
    target_compile_options(zed_core PRIVATE /W4)
endif()

add_library(zed_ui
    src/ui/markdown.cpp
    src/ui/terminal.cpp
    src/ui/theme.cpp
)

target_include_directories(zed_ui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(zed_ui
    PUBLIC
        zed_core
        ftxui::ftxui
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zed_ui PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
elseif (MSVC)
    target_compile_options(zed_ui PRIVATE /W4)
endif()

add_executable(zeda src/main.cpp)
target_compile_definitions(zeda PRIVATE ZEDA_VERSION="${PROJECT_VERSION}")
target_link_libraries(zeda PRIVATE zed_ui)

if (ZEDA_BUILD_DEEPWIKI_PLUGIN)
    add_library(zeda_deepwiki SHARED
        plugins/deepwiki/deepwiki_plugin.cpp
    )
    target_include_directories(zeda_deepwiki PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(zeda_deepwiki PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
        SQLite::SQLite3
    )
    set_target_properties(zeda_deepwiki PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/deepwiki
    )
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(zeda_deepwiki PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    elseif (MSVC)
        target_compile_options(zeda_deepwiki PRIVATE /W4)
    endif()

    set(ZEDA_DEEPWIKI_BUILD_ROOT ${CMAKE_BINARY_DIR}/plugins/deepwiki)
    set(ZEDA_DEEPWIKI_BUILD_RESOURCES ${ZEDA_DEEPWIKI_BUILD_ROOT}/resources)
    file(MAKE_DIRECTORY
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/licenses
    )
    configure_file(
        plugins/deepwiki/zeda-plugin.json.in
        ${ZEDA_DEEPWIKI_BUILD_ROOT}/zeda-plugin.json
        @ONLY
    )
    file(COPY
        plugins/deepwiki/web/index.html
        plugins/deepwiki/web/app.js
        plugins/deepwiki/web/style.css
        DESTINATION ${ZEDA_DEEPWIKI_BUILD_RESOURCES}
    )
    file(COPY ${marked_web_SOURCE_DIR}/lib/marked.umd.js
         DESTINATION ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor)
    file(RENAME
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor/marked.umd.js
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor/marked.js)
    file(COPY ${dompurify_web_SOURCE_DIR}/dist/purify.min.js
         DESTINATION ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor)
    file(RENAME
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor/purify.min.js
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor/purify.js)
    file(COPY ${mermaid_web_SOURCE_DIR}/dist/mermaid.min.js
         DESTINATION ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor)
    file(RENAME
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor/mermaid.min.js
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/vendor/mermaid.js)
    configure_file(${marked_web_SOURCE_DIR}/LICENSE
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/licenses/marked-LICENSE COPYONLY)
    configure_file(${dompurify_web_SOURCE_DIR}/LICENSE
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/licenses/dompurify-LICENSE COPYONLY)
    configure_file(${mermaid_web_SOURCE_DIR}/LICENSE
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/licenses/mermaid-LICENSE COPYONLY)
    configure_file(${cpp_httplib_SOURCE_DIR}/LICENSE
        ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/licenses/cpp-httplib-LICENSE COPYONLY)
endif()

include(GNUInstallDirs)
install(TARGETS zeda RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${ZEDA_DEFAULT_SYSTEM_PROMPT_FILE}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zeda)
install(FILES include/zed/plugins/plugin_sdk.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zeda)
if (ZEDA_BUILD_DEEPWIKI_PLUGIN)
    install(TARGETS zeda_deepwiki
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/zeda/plugins/deepwiki)
    install(FILES ${ZEDA_DEEPWIKI_BUILD_ROOT}/zeda-plugin.json
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/zeda/plugins/deepwiki)
    install(DIRECTORY ${ZEDA_DEEPWIKI_BUILD_RESOURCES}/
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/zeda/plugins/deepwiki/resources)
endif()

include(CTest)

if (BUILD_TESTING)
    add_executable(zed_core_smoke tests/core_smoke.cpp)
    target_link_libraries(zed_core_smoke PRIVATE zed_core)
    add_test(NAME zed_core_smoke COMMAND zed_core_smoke)

    add_executable(zed_modules_smoke tests/modules_smoke.cpp)
    target_link_libraries(zed_modules_smoke PRIVATE zed_ui)
    add_test(NAME zed_modules_smoke COMMAND zed_modules_smoke)

    add_executable(zed_config_smoke tests/config_smoke.cpp)
    target_link_libraries(zed_config_smoke PRIVATE zed_core)
    add_test(NAME zed_config_smoke COMMAND zed_config_smoke)

    add_executable(zed_markdown_smoke tests/markdown_smoke.cpp)
    target_link_libraries(zed_markdown_smoke PRIVATE zed_ui)
    add_test(NAME zed_markdown_smoke COMMAND zed_markdown_smoke)

    add_executable(zed_quick_bash_smoke tests/quick_bash_smoke.cpp)
    target_link_libraries(zed_quick_bash_smoke PRIVATE zed_core)
    add_test(NAME zed_quick_bash_smoke COMMAND zed_quick_bash_smoke)

    add_executable(zed_session_smoke tests/session_smoke.cpp)
    target_link_libraries(zed_session_smoke PRIVATE zed_core)
    add_test(NAME zed_session_smoke COMMAND zed_session_smoke)

    add_executable(zed_opencode_go_smoke tests/opencode_go_smoke.cpp)
    target_link_libraries(zed_opencode_go_smoke PRIVATE zed_core)
    add_test(NAME zed_opencode_go_smoke COMMAND zed_opencode_go_smoke)

    add_executable(zed_fake_clangd tests/fake_clangd.cpp)
    add_executable(zed_fake_opencode tests/fake_opencode.cpp)
    add_executable(zed_fake_model_catalog tests/fake_model_catalog.cpp)

    add_executable(zed_subagent_smoke tests/subagent_smoke.cpp)
    target_link_libraries(zed_subagent_smoke PRIVATE zed_core)
    target_compile_definitions(zed_subagent_smoke PRIVATE
        ZED_TEST_FAKE_CLANGD_PATH="$<TARGET_FILE:zed_fake_clangd>"
        ZED_TEST_FAKE_OPENCODE_PATH="$<TARGET_FILE:zed_fake_opencode>"
    )
    add_dependencies(zed_subagent_smoke zed_fake_clangd zed_fake_opencode)
    add_test(NAME zed_subagent_smoke COMMAND zed_subagent_smoke)

    if (ZEDA_BUILD_DEEPWIKI_PLUGIN)
        add_executable(zed_plugin_smoke tests/plugin_smoke.cpp)
        target_link_libraries(zed_plugin_smoke PRIVATE
            zed_core
            httplib::httplib
        )
        target_compile_definitions(zed_plugin_smoke PRIVATE
            ZED_TEST_PLUGIN_ROOT="${CMAKE_BINARY_DIR}/plugins/deepwiki"
        )
        add_dependencies(zed_plugin_smoke zeda_deepwiki)
        add_test(NAME zed_plugin_smoke COMMAND zed_plugin_smoke)
    endif()

    find_program(ZED_TEST_CLANGD_EXECUTABLE NAMES clangd)
    if (ZED_TEST_CLANGD_EXECUTABLE)
        add_executable(zed_clangd_smoke tests/clangd_smoke.cpp)
        target_link_libraries(zed_clangd_smoke PRIVATE zed_core)
        target_compile_definitions(zed_clangd_smoke PRIVATE
            ZED_TEST_CLANGD_PATH="${ZED_TEST_CLANGD_EXECUTABLE}"
            ZED_TEST_CXX_PATH="${CMAKE_CXX_COMPILER}"
        )
        add_test(NAME zed_clangd_smoke COMMAND zed_clangd_smoke)
    else()
        message(STATUS "clangd not found; skipping the real clangd integration test")
    endif()

    set(ZED_ASSERTING_TEST_TARGETS
        zed_core_smoke
        zed_modules_smoke
        zed_config_smoke
        zed_markdown_smoke
        zed_quick_bash_smoke
        zed_session_smoke
        zed_opencode_go_smoke
        zed_subagent_smoke
    )
    if (TARGET zed_plugin_smoke)
        list(APPEND ZED_ASSERTING_TEST_TARGETS zed_plugin_smoke)
    endif()
    if (TARGET zed_clangd_smoke)
        list(APPEND ZED_ASSERTING_TEST_TARGETS zed_clangd_smoke)
    endif()
    foreach(test_target IN LISTS ZED_ASSERTING_TEST_TARGETS)
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        elseif (MSVC)
            target_compile_options(${test_target} PRIVATE /UNDEBUG)
        endif()
    endforeach()

    add_test(NAME zeda_help COMMAND zeda --help)
    set_tests_properties(zeda_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Usage: zeda"
    )

    add_test(NAME zeda_version COMMAND zeda --version)
    set_tests_properties(zeda_version PROPERTIES
        PASS_REGULAR_EXPRESSION "zeda ${PROJECT_VERSION}"
    )

    add_test(
        NAME zeda_model_refresh_smoke
        COMMAND ${CMAKE_COMMAND}
            "-DZEDA_EXECUTABLE=$<TARGET_FILE:zeda>"
            "-DFAKE_OPENCODE_EXECUTABLE=$<TARGET_FILE:zed_fake_model_catalog>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/model-refresh-smoke"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/model_refresh_smoke.cmake
    )
endif()
