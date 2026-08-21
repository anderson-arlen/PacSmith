cmake_minimum_required(VERSION 3.25)

project(PacSmith VERSION 0.1.0 LANGUAGES CXX)

add_compile_definitions(PACSMITH_VERSION="${PROJECT_VERSION}")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Default current-user installation prefix" FORCE)
endif()

option(PACSMITH_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PACSMITH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_subdirectory(client)
add_subdirectory(server)

install(FILES LICENSE README.md docs/ARCHITECTURE.md docs/IMPLEMENTATION.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/pacsmith)
