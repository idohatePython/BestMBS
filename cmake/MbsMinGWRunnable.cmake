function(mbs_prepare_mingw_runtime)
    if(NOT MINGW OR TARGET mbs_mingw_runtime)
        return()
    endif()

    get_filename_component(mbs_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(mbs_runtime_names
        libstdc++-6.dll
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
    )
    set(mbs_runtime_outputs)
    set(mbs_runtime_sources)
    foreach(name IN LISTS mbs_runtime_names)
        set(source "${mbs_mingw_bin}/${name}")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Missing MinGW runtime from selected toolchain: ${source}")
        endif()
        list(APPEND mbs_runtime_sources "${source}")
        list(APPEND mbs_runtime_outputs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${name}")
    endforeach()

    add_custom_command(
        OUTPUT ${mbs_runtime_outputs}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${mbs_runtime_sources} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        DEPENDS ${mbs_runtime_sources}
        COMMENT "Deploying the selected MinGW runtime beside project executables"
        VERBATIM
    )
    add_custom_target(mbs_mingw_runtime ALL DEPENDS ${mbs_runtime_outputs})
endfunction()

function(mbs_require_mingw_runtime target)
    if(TARGET mbs_mingw_runtime)
        add_dependencies(${target} mbs_mingw_runtime)
    endif()
endfunction()
