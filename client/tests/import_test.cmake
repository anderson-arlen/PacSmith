if(NOT DEFINED PACSMITH_EXE OR NOT DEFINED PACSMITHD_EXE OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "PacSmith import test requires PACSMITH_EXE, PACSMITHD_EXE, SOURCE_DIR, and BINARY_DIR")
endif()

set(runtime_dir "${BINARY_DIR}/import-test-runtime")
set(package_dir "${runtime_dir}/package")
set(data_home "${runtime_dir}/xdg-data")
set(config_home "${runtime_dir}/xdg-config")
set(state_home "${runtime_dir}/xdg-state")
set(runtime_home "${runtime_dir}/xdg-runtime")
set(fixture_dir "${SOURCE_DIR}/tests/fixtures/sample-deb")
set(pid_file "${runtime_dir}/pacsmithd.pid")
set(daemon_log "${runtime_dir}/pacsmithd.log")
file(REMOVE_RECURSE "${runtime_dir}")
file(MAKE_DIRECTORY "${package_dir}" "${data_home}" "${config_home}" "${state_home}" "${runtime_home}")
file(COPY "${fixture_dir}/debian-binary" DESTINATION "${package_dir}")

execute_process(
    COMMAND bsdtar -caf "${package_dir}/control.tar.zst" -C "${fixture_dir}/control" .
    RESULT_VARIABLE control_result ERROR_VARIABLE control_error)
if(NOT control_result EQUAL 0)
    message(FATAL_ERROR "Could not create control archive: ${control_error}")
endif()
execute_process(
    COMMAND bsdtar -caf "${package_dir}/data.tar.zst" -C "${fixture_dir}/data" .
    RESULT_VARIABLE data_result ERROR_VARIABLE data_error)
if(NOT data_result EQUAL 0)
    message(FATAL_ERROR "Could not create data archive: ${data_error}")
endif()
execute_process(
    COMMAND ar r sample.deb debian-binary control.tar.zst data.tar.zst
    WORKING_DIRECTORY "${package_dir}"
    RESULT_VARIABLE ar_result ERROR_VARIABLE ar_error)
if(NOT ar_result EQUAL 0)
    message(FATAL_ERROR "Could not create DEB fixture: ${ar_error}")
endif()

set(test_env
    "XDG_DATA_HOME=${data_home}"
    "XDG_CONFIG_HOME=${config_home}"
    "XDG_STATE_HOME=${state_home}"
    "XDG_RUNTIME_DIR=${runtime_home}"
    "PACSMITH_SECRET_BACKEND=file")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${test_env}
            bash -c "\"${PACSMITHD_EXE}\" >\"${daemon_log}\" 2>&1 & echo $! > \"${pid_file}\""
    RESULT_VARIABLE daemon_start_result)
if(NOT daemon_start_result EQUAL 0)
    message(FATAL_ERROR "could not start pacsmithd")
endif()

set(socket "${runtime_home}/pacsmith/pacsmith.sock")
set(ready FALSE)
foreach(_wait RANGE 1 50)
    if(EXISTS "${socket}")
        set(ready TRUE)
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()
if(NOT ready)
    file(READ "${pid_file}" daemon_pid)
    execute_process(COMMAND kill ${daemon_pid} ERROR_QUIET)
    if(EXISTS "${daemon_log}")
        file(READ "${daemon_log}" daemon_log_text)
    endif()
    message(FATAL_ERROR "pacsmithd did not create ${socket}\n${daemon_log_text}")
endif()

macro(stop_pacsmithd)
    if(EXISTS "${pid_file}")
        file(READ "${pid_file}" daemon_pid)
        string(STRIP "${daemon_pid}" daemon_pid)
        execute_process(COMMAND kill ${daemon_pid} ERROR_QUIET)
    endif()
endmacro()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${test_env}
            "${PACSMITH_EXE}" add "${package_dir}/sample.deb"
    RESULT_VARIABLE add_result OUTPUT_VARIABLE add_output ERROR_VARIABLE add_error)
if(NOT add_result EQUAL 0)
    stop_pacsmithd()
    if(EXISTS "${daemon_log}")
        file(READ "${daemon_log}" daemon_log_text)
    endif()
    message(FATAL_ERROR "pacsmith add failed: ${add_error}\n${add_output}\n${daemon_log_text}")
endif()

if(EXISTS "${data_home}/pacsmith/projects")
    stop_pacsmithd()
    message(FATAL_ERROR "import wrote the legacy library tree")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${test_env}
            "${PACSMITH_EXE}" dependencies pacsmith-smoke
    RESULT_VARIABLE dependencies_result OUTPUT_VARIABLE dependencies_output ERROR_VARIABLE dependencies_error)
if(NOT dependencies_result EQUAL 0 OR NOT dependencies_output MATCHES "gtk3.*Resolved"
   OR NOT dependencies_output MATCHES "unknown-vendor-runtime.*Unresolved")
    stop_pacsmithd()
    message(FATAL_ERROR "Dependency CLI output was incomplete: ${dependencies_error}${dependencies_output}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${test_env}
            "${PACSMITH_EXE}" scripts pacsmith-smoke
    RESULT_VARIABLE scripts_result OUTPUT_VARIABLE scripts_output)
if(NOT scripts_result EQUAL 0 OR NOT scripts_output MATCHES "postinst.*REVIEW REQUIRED")
    stop_pacsmithd()
    message(FATAL_ERROR "Maintainer scripts were not exposed by the CLI")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${test_env}
            "${PACSMITH_EXE}" pkgbuild pacsmith-smoke
    RESULT_VARIABLE pkgbuild_result OUTPUT_VARIABLE pkgbuild_output)
if(NOT pkgbuild_result EQUAL 0)
    stop_pacsmithd()
    message(FATAL_ERROR "pacsmith pkgbuild failed: ${pkgbuild_output}")
endif()
foreach(expected IN ITEMS "pkgname=\"\${_PACSMITH_PKGNAME}\"" "depends=('glibc' 'gtk3')" "options=('!strip' '!debug')")
    string(FIND "${pkgbuild_output}" "${expected}" found)
    if(found EQUAL -1)
        stop_pacsmithd()
        message(FATAL_ERROR "PKGBUILD CLI output was incomplete (${expected}): ${pkgbuild_output}")
    endif()
endforeach()

stop_pacsmithd()
