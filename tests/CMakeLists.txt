add_executable(mbs-sqlite-tests sqlite_tests.cpp)
target_link_libraries(mbs-sqlite-tests PRIVATE mbs::infrastructure)
mbs_set_project_warnings(mbs-sqlite-tests)
mbs_require_mingw_runtime(mbs-sqlite-tests)

add_executable(mbs-abaqus-gateway-tests abaqus_gateway_tests.cpp)
target_link_libraries(mbs-abaqus-gateway-tests PRIVATE mbs::infrastructure)
mbs_set_project_warnings(mbs-abaqus-gateway-tests)
mbs_require_mingw_runtime(mbs-abaqus-gateway-tests)

add_executable(mbs-domain-tests domain_tests.cpp)
target_link_libraries(mbs-domain-tests PRIVATE mbs::domain mbs::application)
mbs_set_project_warnings(mbs-domain-tests)
mbs_require_mingw_runtime(mbs-domain-tests)

add_executable(mbs-material-tests material_tests.cpp)
target_link_libraries(mbs-material-tests PRIVATE mbs::domain)
mbs_set_project_warnings(mbs-material-tests)
mbs_require_mingw_runtime(mbs-material-tests)

add_executable(mbs-application-tests application_tests.cpp)
target_link_libraries(mbs-application-tests PRIVATE mbs::domain mbs::application)
mbs_set_project_warnings(mbs-application-tests)
mbs_require_mingw_runtime(mbs-application-tests)

add_executable(mbs-runtime-tests runtime_tests.cpp)
target_link_libraries(mbs-runtime-tests PRIVATE mbs::runtime)
mbs_set_project_warnings(mbs-runtime-tests)
mbs_require_mingw_runtime(mbs-runtime-tests)

add_executable(mbs-process-runner-tests process_runner_tests.cpp)
target_link_libraries(mbs-process-runner-tests PRIVATE mbs::runtime)
mbs_set_project_warnings(mbs-process-runner-tests)
mbs_require_mingw_runtime(mbs-process-runner-tests)
add_dependencies(mbs-process-runner-tests mbs-worker)
add_test(NAME runtime-protocol COMMAND mbs-runtime-tests)
add_test(NAME worker-health-check COMMAND mbs-worker)
set_tests_properties(worker-health-check PROPERTIES
    PASS_REGULAR_EXPRESSION "TPMS_EVENT:.*C\\+\\+ worker ready"
)
add_test(NAME abaqus-gateway-contracts COMMAND mbs-abaqus-gateway-tests)
add_test(NAME sqlite-contracts COMMAND mbs-sqlite-tests)
add_test(NAME domain-contracts COMMAND mbs-domain-tests)
add_test(NAME material-contracts COMMAND mbs-material-tests)
add_test(NAME application-contracts COMMAND mbs-application-tests)

add_test(
    NAME architecture-isolation
    COMMAND ${CMAKE_COMMAND}
        -DMBS_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_architecture.cmake
)
add_test(NAME process-runner-contracts COMMAND mbs-process-runner-tests $<TARGET_FILE:mbs-worker>)

add_executable(mbs-geometry-tests geometry_tests.cpp)
target_link_libraries(mbs-geometry-tests PRIVATE mbs::geometry)
if(MBS_ENABLE_TETGEN)
    target_compile_definitions(mbs-geometry-tests PRIVATE MBS_TEST_TETGEN)
endif()
mbs_set_project_warnings(mbs-geometry-tests)
mbs_require_mingw_runtime(mbs-geometry-tests)
add_test(NAME geometry-golden-contracts COMMAND mbs-geometry-tests)

add_executable(mbs-optimization-tests optimization_tests.cpp)
target_link_libraries(mbs-optimization-tests PRIVATE mbs::optimization mbs::infrastructure
                                                     mbs::runtime)
mbs_set_project_warnings(mbs-optimization-tests)
mbs_require_mingw_runtime(mbs-optimization-tests)
add_dependencies(mbs-optimization-tests mbs-worker)
add_test(NAME optimization-contracts COMMAND mbs-optimization-tests $<TARGET_FILE:mbs-worker>)

get_filename_component(mbs_test_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
set_tests_properties(
    runtime-protocol
    worker-health-check
    abaqus-gateway-contracts
    sqlite-contracts
    domain-contracts
    material-contracts
    application-contracts
    process-runner-contracts
    geometry-golden-contracts
    optimization-contracts
    PROPERTIES ENVIRONMENT_MODIFICATION
               "PATH=path_list_prepend:${mbs_test_toolchain_bin}"
)
