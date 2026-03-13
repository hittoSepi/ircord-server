##   cmake/functions.cmake
##   Helpers CMake functions and tools

function(string_split INPUT DELIM OUTPUT)
    string(REPLACE "${DELIM}" ";" _result "${INPUT}")
    set(${OUTPUT} "${_result}" PARENT_SCOPE)
endfunction()

function(generate_version_header VERSION_MAJOR VERSION_MINOR VERSION_PATCH VERSION_STRING GIT_VERSION GIT_COMMIT_HASH BUILD_TIMESTAMP)
    set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/version.hpp")

    file(WRITE "${VERSION_HEADER}" "// Auto-generated version header - DO NOT EDIT\n")
    file(APPEND "${VERSION_HEADER}" "#pragma once\n")
    file(APPEND "${VERSION_HEADER}" "#include <string_view>\n\n")
    file(APPEND "${VERSION_HEADER}" "namespace ircord {\n")
    file(APPEND "${VERSION_HEADER}" "inline constexpr std::string_view VERSION_MAJOR = \"${VERSION_MAJOR}\";\n")
    file(APPEND "${VERSION_HEADER}" "inline constexpr std::string_view VERSION_MINOR = \"${VERSION_MINOR}\";\n")
    file(APPEND "${VERSION_HEADER}" "inline constexpr std::string_view VERSION_PATCH = \"${VERSION_PATCH}\";\n")
    file(APPEND "${VERSION_HEADER}" "inline constexpr std::string_view VERSION = \"${VERSION_STRING}\";\n")
    file(APPEND "${VERSION_HEADER}" "inline constexpr std::string_view kVersionString = \"${GIT_VERSION}\";\n")
    file(APPEND "${VERSION_HEADER}" "inline constexpr std::string_view kGitCommitHash = \"${GIT_COMMIT_HASH}\";\n")
    file(APPEND "${VERSION_HEADER}" "inline constexpr std::string_view kBuildTimestamp = \"${BUILD_TIMESTAMP}\";\n")
    file(APPEND "${VERSION_HEADER}" "inline constexpr std::string_view kProjectVersion = \"${VERSION_STRING}\";\n")
    file(APPEND "${VERSION_HEADER}" "} // namespace ircord\n")

    message(STATUS "Generated version header: ${VERSION_HEADER}")
endfunction()

function(generate_version)
    set(PATCH_COUNT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_count")

    if(NOT EXISTS "${PATCH_COUNT_PATH}")
        file(WRITE "${PATCH_COUNT_PATH}" "0.10.0\n")
    endif()

    file(READ "${PATCH_COUNT_PATH}" PATCH_COUNT)
    string(STRIP "${PATCH_COUNT}" PATCH_COUNT)

    if(NOT PATCH_COUNT MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Invalid version format in ${PATCH_COUNT_PATH}: '${PATCH_COUNT}'")
    endif()

    string(REPLACE "." ";" VERSION_LIST "${PATCH_COUNT}")
    list(GET VERSION_LIST 0 VERSION_MAJOR)
    list(GET VERSION_LIST 1 VERSION_MINOR)
    list(GET VERSION_LIST 2 VERSION_PATCH)

    if(MINOR_PATCH)
        math(EXPR VERSION_MINOR "${VERSION_MINOR} + 1")
        set(VERSION_PATCH 0)
        message(STATUS "Minor version bump requested, resetting patch to 0")
    else()
        math(EXPR VERSION_PATCH "${VERSION_PATCH} + 1")
    endif()

    set(PROJECT_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

    string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")

    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()

    if(NOT GIT_COMMIT_HASH)
        set(GIT_COMMIT_HASH "unknown")
    endif()

    if(NOT GIT_VERSION)
        set(GIT_VERSION "${PROJECT_VERSION}")
    endif()

    file(WRITE "${PATCH_COUNT_PATH}" "${PROJECT_VERSION}\n")

    set(PROJECT_VERSION "${PROJECT_VERSION}" PARENT_SCOPE)
    set(PROJECT_VERSION_MAJOR "${VERSION_MAJOR}" PARENT_SCOPE)
    set(PROJECT_VERSION_MINOR "${VERSION_MINOR}" PARENT_SCOPE)
    set(PROJECT_VERSION_PATCH "${VERSION_PATCH}" PARENT_SCOPE)
    set(BUILD_TIMESTAMP "${BUILD_TIMESTAMP}" PARENT_SCOPE)
    set(GIT_COMMIT_HASH "${GIT_COMMIT_HASH}" PARENT_SCOPE)
    set(GIT_VERSION "${GIT_VERSION}" PARENT_SCOPE)

    generate_version_header(
        "${VERSION_MAJOR}"
        "${VERSION_MINOR}"
        "${VERSION_PATCH}"
        "${PROJECT_VERSION}"
        "${GIT_VERSION}"
        "${GIT_COMMIT_HASH}"
        "${BUILD_TIMESTAMP}"
    )

    message(STATUS "IRCord Server Version: ${GIT_VERSION} (${GIT_COMMIT_HASH}) built at ${BUILD_TIMESTAMP}")
endfunction()
