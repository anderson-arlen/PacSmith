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

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${test_env} "${PACSMITH_EXE}" plugin path
    RESULT_VARIABLE plugin_path_result OUTPUT_VARIABLE plugin_path ERROR_VARIABLE plugin_path_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT plugin_path_result EQUAL 0 OR NOT IS_DIRECTORY "${plugin_path}" OR
   NOT EXISTS "${plugin_path}/plugin.json" OR NOT EXISTS "${plugin_path}/mcp.json" OR
   NOT EXISTS "${plugin_path}/skills/pacsmith/SKILL.md")
    stop_pacsmithd()
    message(FATAL_ERROR "Portable Agent Plugin path was invalid: ${plugin_path_error} ${plugin_path}")
endif()

set(mcp_input "${runtime_dir}/mcp-input.jsonl")
file(WRITE "${mcp_input}"
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"elicitation\":{\"form\":{}}},\"clientInfo\":{\"name\":\"pacsmith-test\",\"version\":\"1\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"list_projects\",\"arguments\":{\"query\":\"pacsmith-smoke\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get_project\",\"arguments\":{\"project\":\"pacsmith-smoke\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"set_library_settings\",\"arguments\":{\"weekday\":0}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":\"pacsmith-confirm-1\",\"result\":{\"action\":\"decline\"}}\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${test_env} "${PACSMITH_EXE}" mcp
    INPUT_FILE "${mcp_input}"
    RESULT_VARIABLE mcp_result OUTPUT_VARIABLE mcp_output ERROR_VARIABLE mcp_error)
if(NOT mcp_result EQUAL 0)
    stop_pacsmithd()
    message(FATAL_ERROR "pacsmith mcp failed: ${mcp_error}\n${mcp_output}")
endif()
foreach(expected IN ITEMS
        "\"protocolVersion\":\"2025-11-25\""
        "\"name\":\"get_dependencies\""
        "\"name\":\"get_package_metadata\""
        "\"name\":\"get_release_issues\""
        "\"name\":\"set_dependency_mapping\""
        "\"name\":\"check_updates\""
        "\"name\":\"upsert_harness_profile\""
        "\"readOnlyHint\":true"
        "\"destructiveHint\":true"
        "pacsmith-smoke")
    string(FIND "${mcp_output}" "${expected}" found)
    if(found EQUAL -1)
        stop_pacsmithd()
        message(FATAL_ERROR "MCP output omitted ${expected}: ${mcp_output}")
    endif()
endforeach()
if(mcp_output MATCHES "elicitation/create")
    stop_pacsmithd()
    message(FATAL_ERROR "PacSmith emitted a duplicate MCP elicitation request: ${mcp_output}")
endif()
string(REGEX MATCH "\"release_id\":\"([0-9a-f-]+)\"" release_match "${mcp_output}")
set(release_id "${CMAKE_MATCH_1}")
if(release_id STREQUAL "")
    stop_pacsmithd()
    message(FATAL_ERROR "Could not obtain release ID through MCP: ${mcp_output}")
endif()

set(mcp_write_input "${runtime_dir}/mcp-write-input.jsonl")
file(WRITE "${mcp_write_input}"
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"pacsmith-test\",\"version\":\"1\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"get_release_issues\",\"arguments\":{\"release_id\":\"${release_id}\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"set_dependency_mapping\",\"arguments\":{\"project_name\":\"pacsmith-smoke-bin\",\"release_name\":\"3:1.2.3~beta1-4\",\"dependency\":\"unknown-vendor-runtime\",\"status\":\"ignored\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get_dependencies\",\"arguments\":{\"release_id\":\"${release_id}\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"upsert_harness_profile\",\"arguments\":{\"name\":\"Test harness\",\"executable\":\"agent-cli\",\"arguments\":[\"--prompt\",\"{prompt}\",\"literal;not-shell\"],\"default\":true}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"list_harness_profiles\",\"arguments\":{}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"check_updates\",\"arguments\":{\"project_name\":\"pacsmith-smoke-bin\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"acknowledge_vendor_script\",\"arguments\":{\"project_name\":\"pacsmith-smoke-bin\",\"release_name\":\"3:1.2.3~beta1-4\",\"name\":\"postinst\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"acknowledge_vendor_script\",\"arguments\":{\"project_name\":\"pacsmith-smoke-bin\",\"release_name\":\"3:1.2.3~beta1-4\",\"name\":\"postrm\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"get_release_issues\",\"arguments\":{\"release_id\":\"${release_id}\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"set_package_metadata\",\"arguments\":{\"project_name\":\"pacsmith-smoke-bin\",\"release_name\":\"3:1.2.3~beta1-4\",\"description\":\"Smoke package\",\"homepage\":\"https://vendor.example/smoke\",\"licenses\":[\"MIT\"],\"provides\":[\"smoke-virtual\"],\"conflicts\":[]}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"add_runtime_dependency\",\"arguments\":{\"project_name\":\"pacsmith-smoke-bin\",\"release_name\":\"3:1.2.3~beta1-4\",\"arch_package\":\"libnotify\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\",\"params\":{\"name\":\"get_package_metadata\",\"arguments\":{\"release_id\":\"${release_id}\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\",\"params\":{\"name\":\"get_update_configuration\",\"arguments\":{\"release_id\":\"${release_id}\"}}}\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${test_env} "${PACSMITH_EXE}" mcp
    INPUT_FILE "${mcp_write_input}"
    RESULT_VARIABLE mcp_write_result OUTPUT_VARIABLE mcp_write_output ERROR_VARIABLE mcp_write_error)
if(NOT mcp_write_result EQUAL 0 OR NOT mcp_write_output MATCHES "unknown-vendor-runtime"
   OR NOT mcp_write_output MATCHES "Ignored" OR NOT mcp_write_output MATCHES "Test harness"
   OR NOT mcp_write_output MATCHES "agent-cli" OR NOT mcp_write_output MATCHES "checks"
   OR NOT mcp_write_output MATCHES "postinst" OR NOT mcp_write_output MATCHES "postrm"
   OR NOT mcp_write_output MATCHES "remaining_issue_count"
   OR NOT mcp_write_output MATCHES "review_complete"
   OR NOT mcp_write_output MATCHES "maintenance_complete"
   OR NOT mcp_write_output MATCHES "lastChecked.*20[0-9][0-9]-"
   OR NOT mcp_write_output MATCHES "https://vendor.example/smoke"
   OR NOT mcp_write_output MATCHES "smoke-virtual"
   OR NOT mcp_write_output MATCHES "libnotify")
    stop_pacsmithd()
    message(FATAL_ERROR "MCP domain write/read round trip failed: ${mcp_write_error}\n${mcp_write_output}")
endif()

stop_pacsmithd()
