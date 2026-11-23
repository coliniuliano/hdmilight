# hl_patched_component(<name> <version> <sha256 of the registry zip>)
#
# Generates firmware/.patched/<name>: that exact release of espressif/<name> from the component registry with
# patches/<name>/<name>-<version>-hdmilight.patch applied. Called from firmware/CMakeLists.txt before project(), so
# the directory exists when the component manager resolves override_path in main/idf_component.yml. Pinning the
# version and the archive hash means an upstream release can never change what the patch is applied to; to move to a
# newer release, change the version and hash here and in idf_component.yml and re-make the patch.
# Regenerated only when the version, the archive hash or the patch changes; the download is cached next to it.
function(hl_patched_component name version sha256)
    set(fw_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(url "https://components-file.espressif.com/components/espressif/${name}/${version}/espressif__${name}-v${version}.zip")
    set(patch "${fw_dir}/patches/${name}/${name}-${version}-hdmilight.patch")
    set(work "${fw_dir}/.patched")
    set(out "${work}/${name}")
    set(zip "${work}/espressif__${name}-v${version}.zip")

    file(SHA256 "${patch}" patch_sha)
    set(stamp_want "${version} ${sha256} ${patch_sha}")
    set(stamp_have "")
    if(EXISTS "${out}/.hdmilight_stamp" AND EXISTS "${out}/HDMILIGHT_PATCH.md")
        file(READ "${out}/.hdmilight_stamp" stamp_have)
    endif()
    if(stamp_have STREQUAL stamp_want)
        return()
    endif()

    message(STATUS "hdmilight: generating patched ${name} ${version} in ${out}")
    file(MAKE_DIRECTORY "${work}")
    set(zip_ok FALSE)
    if(EXISTS "${zip}")
        file(SHA256 "${zip}" zip_sha)
        if(zip_sha STREQUAL sha256)
            set(zip_ok TRUE)
        endif()
    endif()
    if(NOT zip_ok)
        file(DOWNLOAD "${url}" "${zip}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS dl)
        list(GET dl 0 dl_rc)
        if(NOT dl_rc EQUAL 0)
            file(REMOVE "${zip}")
            message(FATAL_ERROR "hdmilight: could not download ${name} ${version}: ${dl}")
        endif()
    endif()

    set(tmp "${work}/${name}.tmp")
    file(REMOVE_RECURSE "${tmp}" "${out}")
    file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${tmp}")
    # not part of the component as the registry installs it
    file(REMOVE_RECURSE "${tmp}/examples" "${tmp}/test_app" "${tmp}/host_test" "${tmp}/docs")
    find_program(HL_PATCH_EXE patch)
    if(NOT HL_PATCH_EXE)
        message(FATAL_ERROR "hdmilight: 'patch' is needed to build the patched ${name}")
    endif()
    execute_process(COMMAND "${HL_PATCH_EXE}" -p1 --forward --batch -i "${patch}"
                    WORKING_DIRECTORY "${tmp}" RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "hdmilight: ${patch} does not apply to ${name} ${version}:\n${log}")
    endif()
    # the marker main/CMakeLists.txt checks for: only present once the patch applied cleanly
    configure_file("${patch}" "${tmp}/HDMILIGHT_PATCH.md" COPYONLY)
    file(RENAME "${tmp}" "${out}")
    file(WRITE "${out}/.hdmilight_stamp" "${stamp_want}")
endfunction()
