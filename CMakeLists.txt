cmake_minimum_required(VERSION 3.25)

project(PacSmith LANGUAGES CXX)

set(PACSMITH_VERSION "" CACHE STRING "Version embedded in PacSmith binaries")
if(PACSMITH_VERSION STREQUAL "")
    set(PACSMITH_VERSION "development")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE PACSMITH_GIT_RESULT
            OUTPUT_VARIABLE PACSMITH_GIT_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(PACSMITH_GIT_RESULT EQUAL 0 AND
           PACSMITH_GIT_COMMIT MATCHES "^[0-9a-fA-F]+$")
            set(PACSMITH_VERSION "development+g${PACSMITH_GIT_COMMIT}")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                RESULT_VARIABLE PACSMITH_GIT_STATUS_RESULT
                OUTPUT_VARIABLE PACSMITH_GIT_STATUS
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(PACSMITH_GIT_STATUS_RESULT EQUAL 0 AND NOT PACSMITH_GIT_STATUS STREQUAL "")
                string(APPEND PACSMITH_VERSION ".dirty")
            endif()
        endif()
    endif()
endif()
if(NOT PACSMITH_VERSION MATCHES "^[0-9A-Za-z][0-9A-Za-z.+-]*$")
    message(FATAL_ERROR "PACSMITH_VERSION contains unsupported characters: ${PACSMITH_VERSION}")
endif()
message(STATUS "PacSmith build version: ${PACSMITH_VERSION}")

add_compile_definitions(PACSMITH_VERSION="${PACSMITH_VERSION}")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Default current-user installation prefix" FORCE)
endif()

option(PACSMITH_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PACSMITH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(CTest)
include(GNUInstallDirs)

if(BUILD_TESTING)
    add_test(NAME pacsmith-release-version-tests
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/release-version-test.sh")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_subdirectory(client)
add_subdirectory(server)

install(FILES LICENSE README.md docs/ARCHITECTURE.md docs/IMPLEMENTATION.md docs/RELEASING.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/pacsmith)
