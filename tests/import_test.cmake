if(NOT DEFINED PACSMITH_EXE OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "PacSmith import test requires PACSMITH_EXE, SOURCE_DIR, and BINARY_DIR")
endif()

set(runtime_dir "${BINARY_DIR}/import-test-runtime")
set(package_dir "${runtime_dir}/package")
set(data_home "${runtime_dir}/xdg-data")
set(fixture_dir "${SOURCE_DIR}/tests/fixtures/sample-deb")
file(REMOVE_RECURSE "${runtime_dir}")
file(MAKE_DIRECTORY "${package_dir}" "${data_home}")
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

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "XDG_DATA_HOME=${data_home}"
            "${PACSMITH_EXE}" add "${package_dir}/sample.deb"
    RESULT_VARIABLE add_result OUTPUT_VARIABLE add_output ERROR_VARIABLE add_error)
if(NOT add_result EQUAL 0)
    message(FATAL_ERROR "pacsmith add failed: ${add_error}")
endif()
set(project_dir "${data_home}/pacsmith/projects/pacsmith-smoke")
file(GLOB release_directories "${project_dir}/releases/*")
list(LENGTH release_directories release_count)
if(NOT release_count EQUAL 1)
    message(FATAL_ERROR "Import did not create exactly one package release")
endif()
list(GET release_directories 0 release_dir)
if(NOT EXISTS "${project_dir}/project.json" OR NOT EXISTS "${release_dir}/release.json"
   OR NOT EXISTS "${release_dir}/PKGBUILD" OR NOT EXISTS "${release_dir}/pacsmith.vars"
   OR NOT EXISTS "${release_dir}/sources/sample.deb" OR NOT EXISTS "${release_dir}/sample.deb"
   OR NOT EXISTS "${release_dir}/files/icon.xpm")
    message(FATAL_ERROR "Import did not produce the expected persistent project")
endif()

file(READ "${release_dir}/release.json" project_json)
foreach(expected IN ITEMS "postinst" "postrm" "unknown-vendor-runtime" "etc/apt"
                          "packages.example.invalid" "files/icon.xpm"
                          "usr/share/pixmaps/pacsmith-smoke.xpm")
    string(FIND "${project_json}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "project.json did not preserve expected analysis data: ${expected}")
    endif()
endforeach()
file(READ "${release_dir}/PKGBUILD" pkgbuild)
foreach(expected IN ITEMS "pkgname=\"\${_PACSMITH_PKGNAME}\"" "depends=('glibc' 'gtk3')" "options=('!strip' '!debug')" "data.tar|data.tar.*" "rm -rf -- \"\${pkgdir}/etc/apt\"")
    string(FIND "${pkgbuild}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "PKGBUILD did not contain expected content: ${expected}")
    endif()
endforeach()
file(READ "${release_dir}/pacsmith.vars" identity_vars)
foreach(expected IN ITEMS "_PACSMITH_PKGNAME='pacsmith-smoke-bin'" "_PACSMITH_SOURCE='sample.deb'")
    string(FIND "${identity_vars}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "pacsmith.vars did not contain expected content: ${expected}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "XDG_DATA_HOME=${data_home}"
            "${PACSMITH_EXE}" dependencies pacsmith-smoke
    RESULT_VARIABLE dependencies_result OUTPUT_VARIABLE dependencies_output ERROR_VARIABLE dependencies_error)
if(NOT dependencies_result EQUAL 0 OR NOT dependencies_output MATCHES "gtk3.*Resolved"
   OR NOT dependencies_output MATCHES "unknown-vendor-runtime.*Unresolved")
    message(FATAL_ERROR "Dependency CLI output was incomplete: ${dependencies_error}${dependencies_output}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "XDG_DATA_HOME=${data_home}"
            "${PACSMITH_EXE}" scripts pacsmith-smoke
    RESULT_VARIABLE scripts_result OUTPUT_VARIABLE scripts_output)
if(NOT scripts_result EQUAL 0 OR NOT scripts_output MATCHES "postinst.*REVIEW REQUIRED")
    message(FATAL_ERROR "Maintainer scripts were not exposed by the CLI")
endif()
