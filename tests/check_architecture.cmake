if(NOT DEFINED MBS_SOURCE_DIR)
    message(FATAL_ERROR "MBS_SOURCE_DIR is required")
endif()

set(required_baseline_files
    "${MBS_SOURCE_DIR}/baseline/python/cases.json"
    "${MBS_SOURCE_DIR}/baseline/python/reference/python_baseline.json"
)
foreach(path IN LISTS required_baseline_files)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing local Python baseline snapshot: ${path}")
    endif()
endforeach()

set(required_abaqus_runtime_files
    "${MBS_SOURCE_DIR}/runtime/python/mbs/infrastructure/abaqus/scripts/preprocess.py"
    "${MBS_SOURCE_DIR}/runtime/python/mbs/infrastructure/abaqus/scripts/postprocess.py"
    "${MBS_SOURCE_DIR}/runtime/python/mbs/infrastructure/abaqus/scripts/export_animation.py"
    "${MBS_SOURCE_DIR}/runtime/python/mbs/infrastructure/abaqus/proof.py"
    "${MBS_SOURCE_DIR}/runtime/python/mbs/infrastructure/abaqus/runtime_metadata.py"
)
foreach(path IN LISTS required_abaqus_runtime_files)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing isolated Abaqus runtime asset: ${path}")
    endif()
endforeach()

file(GLOB_RECURSE build_inputs
    "${MBS_SOURCE_DIR}/CMakeLists.txt"
    "${MBS_SOURCE_DIR}/cmake/*.cmake"
    "${MBS_SOURCE_DIR}/apps/*.cpp"
    "${MBS_SOURCE_DIR}/apps/*.hpp"
    "${MBS_SOURCE_DIR}/include/*.hpp"
    "${MBS_SOURCE_DIR}/src/*.cpp"
    "${MBS_SOURCE_DIR}/src/*.hpp"
    "${MBS_SOURCE_DIR}/tests/*.cpp"
    "${MBS_SOURCE_DIR}/tests/*.hpp"
)

string(CONCAT forbidden_version_path "Mechanical-Bonding-Structure " "3.0")
foreach(path IN LISTS build_inputs)
    file(READ "${path}" source)
    string(FIND "${source}" "${forbidden_version_path}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "Build input reaches into Python 3.0: ${path}")
    endif()
endforeach()

file(GLOB_RECURSE runtime_sources
    "${MBS_SOURCE_DIR}/include/mbs/runtime/*.hpp"
    "${MBS_SOURCE_DIR}/src/runtime/*.cpp"
)
set(runtime_forbidden_tokens "#include <Qt" "#include <Q" "sqlite" "mbs/presentation")
foreach(path IN LISTS runtime_sources)
    file(READ "${path}" source)
    string(TOLOWER "${source}" source_lower)
    foreach(token IN LISTS runtime_forbidden_tokens)
        string(TOLOWER "${token}" token_lower)
        string(FIND "${source_lower}" "${token_lower}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "Runtime contains forbidden dependency '${token}': ${path}")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE domain_sources
    "${MBS_SOURCE_DIR}/include/mbs/domain/*.hpp"
    "${MBS_SOURCE_DIR}/src/domain/*.cpp"
)
set(domain_forbidden_tokens
    "#include <Qt"
    "#include <Q"
    "sqlite"
    "filesystem"
    "mbs/application"
    "mbs/runtime"
)
foreach(path IN LISTS domain_sources)
    file(READ "${path}" source)
    string(TOLOWER "${source}" source_lower)
    foreach(token IN LISTS domain_forbidden_tokens)
        string(TOLOWER "${token}" token_lower)
        string(FIND "${source_lower}" "${token_lower}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "Domain contains forbidden dependency '${token}': ${path}")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE application_sources
    "${MBS_SOURCE_DIR}/include/mbs/application/*.hpp"
    "${MBS_SOURCE_DIR}/src/application/*.cpp"
)
set(application_forbidden_tokens "#include <Qt" "#include <Q" "sqlite" "mbs/presentation")
foreach(path IN LISTS application_sources)
    file(READ "${path}" source)
    string(TOLOWER "${source}" source_lower)
    foreach(token IN LISTS application_forbidden_tokens)
        string(TOLOWER "${token}" token_lower)
        string(FIND "${source_lower}" "${token_lower}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "Application contains forbidden dependency '${token}': ${path}")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE infrastructure_sources
    "${MBS_SOURCE_DIR}/include/mbs/infrastructure/*.hpp"
    "${MBS_SOURCE_DIR}/src/infrastructure/*.cpp"
)
set(infrastructure_forbidden_tokens "#include <Qt" "#include <Q" "mbs/presentation")
foreach(path IN LISTS infrastructure_sources)
    file(READ "${path}" source)
    string(TOLOWER "${source}" source_lower)
    foreach(token IN LISTS infrastructure_forbidden_tokens)
        string(TOLOWER "${token}" token_lower)
        string(FIND "${source_lower}" "${token_lower}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "Infrastructure contains forbidden dependency '${token}': ${path}")
        endif()
    endforeach()
endforeach()

message(STATUS "Architecture isolation contract passed")
