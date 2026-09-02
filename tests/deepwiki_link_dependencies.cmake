if (NOT DEFINED PLUGIN_FILE)
    message(FATAL_ERROR "PLUGIN_FILE is required")
endif()

file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES "${PLUGIN_FILE}"
    RESOLVED_DEPENDENCIES_VAR resolved_dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved_dependencies
)

foreach(dependency IN LISTS resolved_dependencies unresolved_dependencies)
    if (dependency MATCHES
            "(CFNetwork|Security\\.framework|libbrotli|libcrypto|libssl|libzstd)")
        message(FATAL_ERROR
            "DeepWiki has an unexpected TLS or compression dependency: ${dependency}")
    endif()
endforeach()
