cmake_minimum_required(VERSION 3.25)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

project(
    MechanicalBondingStructure
    VERSION 4.0.0
    DESCRIPTION "C++20 TPMS mechanical bonding design and optimization platform"
    LANGUAGES CXX
)

option(MBS_BUILD_GUI "Build the Qt 6 desktop application" ON)
option(MBS_BUILD_TESTS "Build CTest regression tests" ON)
option(MBS_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(MBS_ENABLE_TETGEN "Build the AGPL/commercial TetGen 1.6 volume-mesh backend" ON)
option(MBS_ENABLE_CGAL "Build the GPL/commercial CGAL mesh diagnostics and repair backend" ON)
set(MBS_SQLITE_ROOT "" CACHE PATH "Prefix containing SQLite headers and libraries")
set(MBS_VTK_DIR "" CACHE PATH "Directory containing VTKConfig.cmake")
set(MBS_EIGEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/eigen" CACHE PATH
    "Eigen 3.4 source directory")
set(MBS_CGAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgal" CACHE PATH
    "Directory containing CGALConfig.cmake")
set(MBS_MANIFOLD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/manifold" CACHE PATH
    "Manifold source directory")
set(MBS_TETGEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tetgen" CACHE PATH
    "TetGen source directory")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MbsWarnings)
include(MbsMinGWRunnable)
mbs_prepare_mingw_runtime()

if(MBS_SQLITE_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${MBS_SQLITE_ROOT}")
endif()
find_package(SQLite3 REQUIRED)
if(TARGET SQLite::SQLite3 AND NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

if(MBS_ENABLE_CGAL)
    if(NOT EXISTS "${MBS_EIGEN_DIR}/Eigen/Core" OR NOT EXISTS "${MBS_CGAL_DIR}/CGALConfig.cmake")
        message(FATAL_ERROR
            "CGAL/Eigen sources are missing. Run tools/bootstrap_dependencies.ps1 "
            "or set MBS_EIGEN_DIR and MBS_CGAL_DIR explicitly.")
    endif()
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    set_target_properties(Eigen3::Eigen PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${MBS_EIGEN_DIR}"
        INTERFACE_COMPILE_DEFINITIONS EIGEN_MPL2_ONLY
    )
    set(Eigen3_FOUND TRUE)
    set(Eigen3_VERSION 3.4.0)
    set(CGAL_DIR "${MBS_CGAL_DIR}" CACHE PATH "Directory containing CGALConfig.cmake" FORCE)
    # Keep the canonical MinGW build self-contained: Boost.Multiprecision avoids
    # importing an MSVC-built GMP/MPFR ABI into the Qt MinGW process.
    set(CGAL_CMAKE_EXACT_NT_BACKEND BOOST_BACKEND CACHE STRING
        "Exact number backend used by CGAL" FORCE)
    find_package(CGAL 6.0 CONFIG REQUIRED)
    include(CGAL_Eigen3_support)
    if(NOT TARGET CGAL::Eigen3_support)
        message(FATAL_ERROR "CGAL adaptive remeshing requires the vendored Eigen 3.4 headers")
    endif()
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build bundled geometry libraries statically" FORCE)
set(MANIFOLD_CROSS_SECTION OFF CACHE BOOL "MBS only needs the 3D Manifold kernel" FORCE)
set(MANIFOLD_TEST OFF CACHE BOOL "Use MBS geometry contract tests" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "Do not build Python bindings" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "Do not build C bindings" FORCE)
set(MANIFOLD_PAR OFF CACHE BOOL "Avoid an additional TBB runtime" FORCE)
set(MANIFOLD_DOWNLOADS OFF CACHE BOOL "All dependencies must be prepared explicitly" FORCE)
if(NOT EXISTS "${MBS_MANIFOLD_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Manifold sources are missing. Run tools/bootstrap_dependencies.ps1 "
        "or set MBS_MANIFOLD_DIR explicitly.")
endif()
add_subdirectory("${MBS_MANIFOLD_DIR}" "${CMAKE_BINARY_DIR}/_deps/manifold" EXCLUDE_FROM_ALL)

if(MBS_BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui OpenGL OpenGLWidgets Widgets)
    if(MBS_VTK_DIR)
        set(VTK_DIR "${MBS_VTK_DIR}")
    endif()
    find_package(VTK 9.5 REQUIRED COMPONENTS
        CommonCore CommonDataModel FiltersCore FiltersGeometry FiltersSources
        GUISupportQt InteractionStyle IOGeometry IOLegacy IOXML
        RenderingAnnotation RenderingCore RenderingOpenGL2)
endif()

if(MBS_ENABLE_TETGEN)
    if(NOT EXISTS "${MBS_TETGEN_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "TetGen sources are missing. Run tools/bootstrap_dependencies.ps1 "
            "or set MBS_TETGEN_DIR explicitly.")
    endif()
    set(BUILD_EXECUTABLE OFF CACHE BOOL "Do not build the TetGen CLI" FORCE)
    set(BUILD_LIBRARY ON CACHE BOOL "Build the embedded TetGen library" FORCE)
    add_subdirectory("${MBS_TETGEN_DIR}" "${CMAKE_BINARY_DIR}/_deps/tetgen" EXCLUDE_FROM_ALL)
    target_include_directories(tetgen PUBLIC "${MBS_TETGEN_DIR}")
    target_compile_definitions(tetgen PUBLIC TETLIBRARY)
endif()

add_library(mbs_domain STATIC
    src/domain/DesignParameters.cpp
    src/domain/Material.cpp
    src/domain/Validation.cpp
    src/domain/Workflow.cpp
)
add_library(mbs::domain ALIAS mbs_domain)
target_include_directories(mbs_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
mbs_set_project_warnings(mbs_domain)

add_library(mbs_geometry STATIC
    src/geometry/ContactRisk.cpp
    src/geometry/GeometryKernel.cpp
    src/geometry/MeshDoctor.cpp
)
add_library(mbs::geometry ALIAS mbs_geometry)
target_include_directories(mbs_geometry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(mbs_geometry SYSTEM PRIVATE
    "${MBS_MANIFOLD_DIR}/include"
    "${MBS_TETGEN_DIR}"
)
target_link_libraries(mbs_geometry PUBLIC mbs::domain manifold::manifold)
if(MBS_ENABLE_TETGEN)
    target_link_libraries(mbs_geometry PRIVATE tetgen)
    target_compile_definitions(mbs_geometry PRIVATE MBS_ENABLE_TETGEN)
endif()
if(MBS_ENABLE_CGAL)
    target_link_libraries(mbs_geometry PRIVATE CGAL::CGAL CGAL::Eigen3_support)
    target_compile_definitions(mbs_geometry PRIVATE MBS_ENABLE_CGAL)
endif()
if(MBS_BUILD_GUI)
    target_link_libraries(mbs_geometry PRIVATE VTK::CommonCore VTK::CommonDataModel VTK::IOXML)
    target_compile_definitions(mbs_geometry PRIVATE MBS_ENABLE_VTK_IO)
endif()
if(MINGW)
    # CGAL's remeshing/simplification templates can exceed the PE/COFF default
    # section count, especially in Debug builds.  Big-object mode is an
    # assembler format switch; it does not change the runtime ABI.
    target_compile_options(mbs_geometry PRIVATE -Wa,-mbig-obj)
    # DWARF plus the deeply nested CGAL exact-kernel template names can still
    # overflow MinGW's COFF string table.  Keep ordinary geometry sources
    # debuggable, but compile this template-heavy translation unit without
    # debug metadata.
    set_source_files_properties(src/geometry/MeshDoctor.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O1>;$<$<CONFIG:Debug>:-g0>;$<$<CONFIG:Debug>:-fno-var-tracking>"
    )
endif()
mbs_set_project_warnings(mbs_geometry)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # CGAL 6 Adaptive_sizing_field currently triggers GCC's
    # -Wmaybe-uninitialized in its dynamic property-map adapter even though
    # the default value is constructed by the library. Keep project warnings
    # as errors without promoting that third-party false positive.
    target_compile_options(mbs_geometry PRIVATE -Wno-maybe-uninitialized)
endif()

add_library(mbs_application STATIC
    src/application/Services.cpp
    src/application/TaskLifecycleService.cpp
    src/application/TaskService.cpp
)
add_library(mbs::application ALIAS mbs_application)
target_include_directories(mbs_application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mbs_application PUBLIC mbs::domain)
mbs_set_project_warnings(mbs_application)

add_library(mbs_optimization STATIC
    src/optimization/GaussianProcessOptimizer.cpp
)
add_library(mbs::optimization ALIAS mbs_optimization)
target_include_directories(mbs_optimization PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mbs_optimization PUBLIC mbs::application)
mbs_set_project_warnings(mbs_optimization)

add_library(mbs_runtime STATIC
    src/runtime/EventEnvelope.cpp
    src/runtime/ProcessRunner.cpp
    src/runtime/WorkerSession.cpp
)
add_library(mbs::runtime ALIAS mbs_runtime)
target_include_directories(mbs_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
mbs_set_project_warnings(mbs_runtime)

add_library(mbs_infrastructure STATIC
    src/infrastructure/abaqus/AbaqusGateway.cpp
    src/infrastructure/runtime/WorkerTaskBridge.cpp
    src/infrastructure/sqlite/Database.cpp
    src/infrastructure/sqlite/DatabaseManager.cpp
    src/infrastructure/sqlite/Repositories.cpp
)
add_library(mbs::infrastructure ALIAS mbs_infrastructure)
target_include_directories(mbs_infrastructure PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mbs_infrastructure PUBLIC mbs::application mbs::runtime SQLite3::SQLite3)
mbs_set_project_warnings(mbs_infrastructure)

add_executable(mbs-worker apps/worker/main.cpp)
target_link_libraries(mbs-worker PRIVATE mbs::infrastructure mbs::geometry mbs::optimization)
mbs_set_project_warnings(mbs-worker)
mbs_require_mingw_runtime(mbs-worker)

if(MBS_BUILD_GUI)
    qt_standard_project_setup()
    add_executable(mbs-gui WIN32
        apps/gui/main.cpp
        src/presentation/ApplicationContext.cpp
        src/presentation/ApplicationContext.hpp
        src/presentation/MainWindow.cpp
        src/presentation/MainWindow.hpp
        src/presentation/MeshViewport.cpp
        src/presentation/MeshViewport.hpp
        src/presentation/Pages.cpp
        src/presentation/Pages.hpp
        src/presentation/StoragePaths.cpp
        src/presentation/StoragePaths.hpp
        src/presentation/TaskController.cpp
        src/presentation/TaskController.hpp
        src/presentation/WorkflowWidgets.cpp
        src/presentation/WorkflowWidgets.hpp
    )
    target_include_directories(mbs-gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(mbs-gui PRIVATE
        MBS_PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mbs-gui PRIVATE
        mbs::application
        mbs::infrastructure
        mbs::runtime
        mbs::optimization
        Qt6::Core
        Qt6::Gui
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Widgets
        ${VTK_LIBRARIES}
    )
    vtk_module_autoinit(TARGETS mbs-gui MODULES ${VTK_LIBRARIES})
    mbs_set_project_warnings(mbs-gui)
    mbs_require_mingw_runtime(mbs-gui)

    # A build-tree GUI must be runnable directly. Without local Qt DLLs,
    # Windows may resolve an unrelated Qt installation from PATH and fail with
    # missing ABI entries such as qt_assert_x or QPersistentModelIndex helpers.
    if(WIN32)
        get_target_property(mbs_qmake_executable Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(mbs_qt_bin "${mbs_qmake_executable}" DIRECTORY)
        find_program(MBS_WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${mbs_qt_bin}" REQUIRED)
        add_custom_command(TARGET mbs-gui POST_BUILD
            COMMAND "${MBS_WINDEPLOYQT_EXECUTABLE}"
                    --no-translations --no-compiler-runtime "$<TARGET_FILE:mbs-gui>"
            COMMENT "Deploying the selected Qt runtime beside mbs-gui"
            VERBATIM
        )
    endif()

    get_filename_component(mbs_vtk_prefix "${VTK_DIR}/../../.." ABSOLUTE)
    file(GLOB mbs_vtk_runtime_dlls "${mbs_vtk_prefix}/bin/*.dll")
    if(MINGW AND NOT mbs_vtk_runtime_dlls)
        message(FATAL_ERROR "VTK runtime DLLs were not found under ${mbs_vtk_prefix}/bin")
    endif()
    if(mbs_vtk_runtime_dlls)
    add_custom_target(mbs_abaqus_runtime_assets ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/python"
                "${CMAKE_BINARY_DIR}/share/mbs/python"
        COMMENT "Deploying isolated Abaqus runtime beside the GUI"
        VERBATIM
    )
    add_dependencies(mbs-gui mbs_abaqus_runtime_assets)
    add_custom_command(TARGET mbs-gui POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    ${mbs_vtk_runtime_dlls} "$<TARGET_FILE_DIR:mbs-gui>"
            COMMENT "Deploying VTK runtime beside mbs-gui"
            VERBATIM
        )
        install(FILES ${mbs_vtk_runtime_dlls} DESTINATION bin)
    endif()

    install(TARGETS mbs-gui RUNTIME DESTINATION bin)
    qt_generate_deploy_app_script(
        TARGET mbs-gui
        OUTPUT_SCRIPT mbs_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT "${mbs_deploy_script}")
endif()

install(TARGETS mbs-worker RUNTIME DESTINATION bin)
install(FILES LICENSE.txt README.md DESTINATION .)
install(DIRECTORY runtime/python/ DESTINATION share/mbs/python
        FILES_MATCHING PATTERN "*.py")
install(FILES "${MBS_MANIFOLD_DIR}/LICENSE"
        DESTINATION share/licenses/manifold
        RENAME LICENSE.txt)
if(MBS_ENABLE_TETGEN)
    install(FILES "${MBS_TETGEN_DIR}/LICENSE"
            DESTINATION share/licenses/tetgen
            RENAME LICENSE.txt)
endif()
if(MBS_ENABLE_CGAL)
    install(FILES "${MBS_CGAL_DIR}/LICENSE.md"
            DESTINATION share/licenses/cgal
            RENAME LICENSE.md)
    install(FILES "${MBS_EIGEN_DIR}/COPYING.MPL2"
            DESTINATION share/licenses/eigen
            RENAME COPYING.MPL2.txt)
endif()
install(FILES docs/THIRD_PARTY_GEOMETRY.md
        DESTINATION share/licenses)

if(MBS_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()
