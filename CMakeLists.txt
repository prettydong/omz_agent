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

add_library(zed_core
    src/app/config.cpp
    src/core/agent_loop.cpp
    src/core/context.cpp
    src/core/model_context_controller.cpp
    src/core/session_store.cpp
    src/core/tool_registry.cpp
    src/providers/opencode_go_model.cpp
    src/session/jsonl_session_store.cpp
    src/extensions/extension_registry.cpp
    src/extensions/quick_bash_input.cpp
    src/skills/skill_registry.cpp
    src/tools/basic_tools.cpp
)

target_include_directories(zed_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(zed_core
    PUBLIC
        nlohmann_json::nlohmann_json
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

include(GNUInstallDirs)
install(TARGETS zeda RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

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

    add_test(NAME zeda_help COMMAND zeda --help)
    set_tests_properties(zeda_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Usage: zeda"
    )

    add_test(NAME zeda_version COMMAND zeda --version)
    set_tests_properties(zeda_version PROPERTIES
        PASS_REGULAR_EXPRESSION "zeda ${PROJECT_VERSION}"
    )
endif()
