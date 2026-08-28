set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Widgets Concurrent Test)
find_package(LibArchive REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LibSecret IMPORTED_TARGET libsecret-1)
pkg_check_modules(LibAlpm REQUIRED IMPORTED_TARGET libalpm)

function(pacsmith_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
            -Wshadow -Wformat=2 -Wundef -Wcast-qual -Wnon-virtual-dtor)
        if(PACSMITH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(PACSMITH_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

add_library(pacsmith_core STATIC
    src/core/agent_skill.cpp
    src/core/app_settings.cpp
    src/core/harness_launcher.cpp
    src/core/background_updates.cpp
    src/core/model.cpp
    src/core/http_transport.cpp
    src/core/library_events.cpp
    src/core/daemon_control.cpp
    src/core/domain_validation.cpp
    src/core/release_review.cpp
    src/core/enrollment.cpp
    src/core/library_client.cpp
    src/core/package_artifact.cpp
    src/core/control_parser.cpp
    src/core/credential_store.cpp
    src/core/apt_sources.cpp
    src/core/apt_repository.cpp
    src/core/apt_update_service.cpp
    src/core/apt_update_service.hpp
    src/core/dependency_parser.cpp
    src/core/lifecycle_validator.cpp
    src/core/managed_package.cpp
    src/core/path_safety.cpp
    src/core/repository_trust.cpp
    src/core/repository_key_download_service.cpp
    src/core/rpm_analyzer.cpp
    src/core/rpm_repository.cpp
    src/core/rpm_update_service.cpp
    src/core/rpm_update_service.hpp
    src/core/script_evidence.cpp
    src/core/source_analyzer.cpp
    src/core/system_package_query.cpp
    src/core/payload_review.cpp
    src/core/payload_inspector.cpp
    src/core/deb_analyzer.cpp
    src/core/deb_download_service.cpp
    src/core/direct_url_update_service.cpp
    src/core/direct_url_update_service.hpp
    src/core/github_update_service.cpp
    src/core/remote_import_service.cpp
    src/core/remote_import_service.hpp
    src/core/project_store/project_store.cpp
    src/core/project_store/internal.cpp
    src/core/project_store/import.cpp
    src/core/pkgbuild_generator.cpp
    src/core/pkgbuild_install_plan.cpp
    src/core/process_services.cpp
    src/core/terminal_install_service.cpp
    src/core/terminal_install_service.hpp
    src/core/update_source.cpp
    src/core/update_check_runner.cpp)
target_include_directories(pacsmith_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pacsmith_core PUBLIC Qt6::Core Qt6::Network CURL::libcurl PRIVATE LibArchive::LibArchive PkgConfig::LibAlpm)
if(LibSecret_FOUND)
    target_link_libraries(pacsmith_core PRIVATE PkgConfig::LibSecret)
    target_compile_definitions(pacsmith_core PRIVATE PACSMITH_HAS_LIBSECRET=1)
endif()
qt_add_resources(pacsmith_core core_resources
    PREFIX "/pacsmith"
    BASE resources
    FILES resources/dependency-mappings.json)
pacsmith_target_defaults(pacsmith_core)

add_executable(pacsmith
    src/cli/main.cpp
    src/cli/agent_integration.cpp
    src/cli/admin.cpp
    src/mcp/server.cpp
    src/mcp/permission_policy.cpp)
target_link_libraries(pacsmith PRIVATE pacsmith_core Qt6::Core)
target_compile_definitions(pacsmith PRIVATE
    PACSMITH_BUILD_DIR="${CMAKE_BINARY_DIR}"
    PACSMITH_PLUGIN_DIR="${CMAKE_INSTALL_FULL_DATADIR}/pacsmith/agent-plugin"
    PACSMITH_SOURCE_PLUGIN_DIR="${CMAKE_SOURCE_DIR}/agent-plugin")
pacsmith_target_defaults(pacsmith)

add_executable(pacsmith-gui
    src/gui/main.cpp
    src/gui/age_unlock.cpp
    src/gui/appearance.cpp
    src/gui/application_session.cpp
    src/gui/command_progress_dialog.cpp
    src/gui/connection_dialog.cpp
    src/gui/desktop_entry_highlighter.cpp
    src/gui/gui_instance.cpp
    src/gui/import_worker.cpp
    src/gui/reanalyze_worker.cpp
    src/gui/wheel_scroll_guard.cpp
    src/gui/main_window/main_window.hpp
    src/gui/main_window/main_window.cpp
    src/gui/main_window/help_widgets.cpp
    src/gui/main_window/support.cpp
    src/gui/main_window/pages.cpp
    src/gui/main_window/import.cpp
    src/gui/main_window/dashboard.cpp
    src/gui/main_window/repository.cpp
    src/gui/main_window/library_sync.cpp
    src/gui/main_window/source.cpp
    src/gui/main_window/payload.cpp
    src/gui/main_window/recipe.cpp
    src/gui/main_window/settings.cpp
    src/gui/main_window/actions.cpp
    src/gui/pkgbuild_highlighter.cpp
    src/gui/shell_highlighter.cpp)
target_link_libraries(pacsmith-gui PRIVATE pacsmith_core Qt6::Widgets Qt6::Concurrent)
qt_add_resources(pacsmith-gui gui_resources
    PREFIX "/pacsmith"
    BASE resources
    FILES
        resources/icons/pacsmith.png
        resources/icons/pacsmith-hero.png
        resources/icons/pacsmith-tray.png)
qt_add_resources(pacsmith-gui license_resources
    PREFIX "/pacsmith"
    BASE ${CMAKE_SOURCE_DIR}
    FILES ${CMAKE_SOURCE_DIR}/LICENSE)
pacsmith_target_defaults(pacsmith-gui)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/packaging)
configure_file(packaging/pacsmith.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/packaging/pacsmith.desktop @ONLY)
install(TARGETS pacsmith pacsmith-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/packaging/pacsmith.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY resources/icons/hicolor
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/agent-plugin/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pacsmith/agent-plugin)

if(BUILD_TESTING)
    add_executable(pacsmith-tests
        tests/core_tests/core_tests.hpp
        tests/core_tests/core_tests.cpp
        tests/core_tests/parser.cpp
        tests/core_tests/library_events.cpp
        tests/core_tests/direct_url_update.cpp
        tests/core_tests/remote_import.cpp
        tests/core_tests/script_evidence.cpp
        tests/core_tests/repository.cpp
        tests/core_tests/project_persistence.cpp
        tests/core_tests/mcp_harness.cpp
        tests/core_tests/pkgbuild.cpp
        tests/core_tests/analyzer.cpp
        tests/core_tests/process_credentials.cpp
        src/mcp/server.cpp
        src/mcp/permission_policy.cpp)
    target_include_directories(pacsmith-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_tests)
    target_link_libraries(pacsmith-tests PRIVATE pacsmith_core Qt6::Test)
    target_compile_definitions(pacsmith-tests PRIVATE
        PACSMITH_SOURCE_PLUGIN_DIR="${CMAKE_SOURCE_DIR}/agent-plugin")
    pacsmith_target_defaults(pacsmith-tests)
    add_test(NAME pacsmith-core-tests COMMAND pacsmith-tests)
    add_executable(pacsmith-gui-tests
        tests/gui_tests/wheel_scroll_guard_test.cpp
        src/gui/appearance.cpp
        src/gui/wheel_scroll_guard.cpp)
    target_include_directories(pacsmith-gui-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(pacsmith-gui-tests PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Test)
    pacsmith_target_defaults(pacsmith-gui-tests)
    add_test(NAME pacsmith-gui-tests COMMAND pacsmith-gui-tests)
    set_tests_properties(pacsmith-gui-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME pacsmith-import-test
        COMMAND ${CMAKE_COMMAND}
            "-DPACSMITH_EXE=$<TARGET_FILE:pacsmith>"
            "-DPACSMITHD_EXE=${CMAKE_BINARY_DIR}/pacsmithd"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/import_test.cmake)
endif()
