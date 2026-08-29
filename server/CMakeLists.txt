find_program(GO_EXECUTABLE go REQUIRED)

set(PACSMITHD_OUTPUT "${CMAKE_BINARY_DIR}/pacsmithd")
set(PACSMITHD_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB_RECURSE PACSMITHD_GO_SOURCES CONFIGURE_DEPENDS
    "${PACSMITHD_MODULE_DIR}/*.go"
    "${PACSMITHD_MODULE_DIR}/*.sql"
    "${PACSMITHD_MODULE_DIR}/*.json"
    "${PACSMITHD_MODULE_DIR}/go.mod")
list(APPEND PACSMITHD_GO_SOURCES
    "${PACSMITHD_MODULE_DIR}/sqlc.yaml")
if(EXISTS "${PACSMITHD_MODULE_DIR}/go.sum")
    list(APPEND PACSMITHD_GO_SOURCES "${PACSMITHD_MODULE_DIR}/go.sum")
endif()

add_custom_command(
    OUTPUT "${PACSMITHD_OUTPUT}"
    COMMAND "${GO_EXECUTABLE}" build
        -trimpath
        -ldflags "-X github.com/anderson-arlen/pacsmith/server/internal/version.Version=${PACSMITH_VERSION}"
        -o "${PACSMITHD_OUTPUT}"
        ./cmd/pacsmithd
    WORKING_DIRECTORY "${PACSMITHD_MODULE_DIR}"
    DEPENDS ${PACSMITHD_GO_SOURCES}
    COMMENT "Building pacsmithd"
    VERBATIM)

add_custom_target(pacsmithd ALL DEPENDS "${PACSMITHD_OUTPUT}")
install(PROGRAMS "${PACSMITHD_OUTPUT}" DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(packaging/pacsmithd.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/pacsmithd.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pacsmithd.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user)

if(BUILD_TESTING)
    add_test(NAME pacsmithd-go-tests
        COMMAND "${GO_EXECUTABLE}" test ./...
        WORKING_DIRECTORY "${PACSMITHD_MODULE_DIR}")
    set_tests_properties(pacsmithd-go-tests PROPERTIES
        ENVIRONMENT "GOCACHE=${CMAKE_BINARY_DIR}/go-cache")
endif()
