cmake_minimum_required(VERSION 3.25)
project(behaviour_camera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export a compile database so clangd / IntelliSense work without guessing flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# DEPENDENCIES (vcpkg-managed)
# ============================================================================
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc)
find_package(glfw3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

# ============================================================================
# SPINNAKER SDK (vendor-provided, not in vcpkg)
# ============================================================================
set(SPINNAKER_ROOT "C:/Program Files/Teledyne/Spinnaker"
    CACHE PATH "Root of the Teledyne Spinnaker SDK installation")

find_path(SPINNAKER_INCLUDE_DIR
    NAMES Spinnaker.h
    HINTS "${SPINNAKER_ROOT}/include"
    NO_DEFAULT_PATH
)

# The SDK ships separate import libraries per CRT. vs2015 holds the v140 core
# libraries; vs2017 only has the GPU extras, so v140 is the right choice even on
# much newer toolsets (MSVC v140 through v14x are binary compatible).
find_library(SPINNAKER_LIBRARY_RELEASE
    NAMES Spinnaker_v140
    HINTS "${SPINNAKER_ROOT}/lib64/vs2015"
    NO_DEFAULT_PATH
)
find_library(SPINNAKER_LIBRARY_DEBUG
    NAMES Spinnakerd_v140
    HINTS "${SPINNAKER_ROOT}/lib64/vs2015"
    NO_DEFAULT_PATH
)

if (NOT SPINNAKER_INCLUDE_DIR OR NOT SPINNAKER_LIBRARY_RELEASE)
    message(FATAL_ERROR
        "Could not find the Spinnaker SDK under '${SPINNAKER_ROOT}'.\n"
        "Install it, or point SPINNAKER_ROOT at it:\n"
        "  cmake --preset <preset> -DSPINNAKER_ROOT=\"C:/path/to/Spinnaker\"\n"
        "Looked for include/Spinnaker.h and lib64/vs2015/Spinnaker_v140.lib."
    )
endif()

add_library(Spinnaker::Spinnaker UNKNOWN IMPORTED)
set_target_properties(Spinnaker::Spinnaker PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SPINNAKER_INCLUDE_DIR}"
    IMPORTED_LOCATION "${SPINNAKER_LIBRARY_RELEASE}"
    IMPORTED_CONFIGURATIONS "RELEASE"
)

# Without this, a Debug build links the release Spinnaker library against the
# debug CRT, which misbehaves in ways that look like camera faults.
if (SPINNAKER_LIBRARY_DEBUG)
    set_property(TARGET Spinnaker::Spinnaker APPEND PROPERTY IMPORTED_CONFIGURATIONS "DEBUG")
    set_target_properties(Spinnaker::Spinnaker PROPERTIES
        IMPORTED_LOCATION_DEBUG "${SPINNAKER_LIBRARY_DEBUG}"
        IMPORTED_LOCATION_RELEASE "${SPINNAKER_LIBRARY_RELEASE}"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RELEASE"
        MAP_IMPORTED_CONFIG_MINSIZEREL "RELEASE"
    )
else()
    message(WARNING "Spinnakerd_v140.lib not found; Debug builds will link the release SDK.")
endif()

# ============================================================================
# SHARED SETTINGS
# ============================================================================
add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    # /EHsc is not optional here. MSVC does not enable unwind semantics unless
    # asked, and CMake 4.x no longer passes it by default the way earlier versions
    # did. Without it the compiler warns C4530 and, more to the point, destructors
    # are not guaranteed to run while an exception propagates — which this program
    # depends on, since every camera fault arrives as a thrown Spinnaker::Exception
    # and the handlers above it expect frames and files to be released on the way out.
    $<$<CXX_COMPILER_ID:MSVC>:/EHsc;/W4;/permissive-;/MP;/utf-8>
)
# Deliberately NOT /Zc:__cplusplus, tempting though it is. SpinnakerPlatform.h
# switches on `__cplusplus >= 201402L`, and that branch defines its deprecation
# macros as `enum [[deprecated(msg)]]` — an attribute position MSVC rejects
# outright (error C3837), so every file including Spinnaker.h fails to compile.
# With the flag absent, MSVC reports the legacy 199711L, the header takes its
# __declspec path instead, and everything builds. The C++ standard version itself
# is unaffected: that comes from CMAKE_CXX_STANDARD above.
# NOMINMAX keeps windows.h from defining min/max as macros, which breaks
# std::min/std::max and several standard headers.
target_compile_definitions(project_options INTERFACE
    $<$<PLATFORM_ID:Windows>:NOMINMAX;WIN32_LEAN_AND_MEAN>
)

# Copies the DLLs that a target needs next to its .exe, so it runs from the
# build tree without depending on PATH. The generator-expression guard is the
# documented way to skip the command when a target needs no DLLs at all.
function(copy_runtime_dlls target)
    if (WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target}>;$<TARGET_FILE_DIR:${target}>>"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endif()
endfunction()

# ============================================================================
# TARGETS
# ============================================================================
add_executable(behaviour_camera src/main.cpp)
target_link_libraries(behaviour_camera PRIVATE
    project_options
    opencv_core
    opencv_imgproc
    glfw
    nlohmann_json::nlohmann_json
    OpenGL::GL
    Spinnaker::Spinnaker
)
copy_runtime_dlls(behaviour_camera)

# Read-only diagnostic: enumerates cameras and measures capture rate without
# touching the disk, so camera/link problems can be separated from write problems.
add_executable(camera_probe tools/camera_probe.cpp)
target_link_libraries(camera_probe PRIVATE
    project_options
    Spinnaker::Spinnaker
)
copy_runtime_dlls(camera_probe)

# ============================================================================
# INSTALLATION
# ============================================================================
install(TARGETS behaviour_camera camera_probe RUNTIME DESTINATION bin)

# vcpkg's own DLLs (OpenCV, GLFW).
install(FILES $<TARGET_RUNTIME_DLLS:behaviour_camera> DESTINATION bin)

# The Spinnaker installer already puts its runtime on the system PATH, so this
# is off by default: copying it adds ~300 MB of DLLs, most of them GUI code this
# project never loads. Turn it on for a fully self-contained package.
option(INSTALL_SPINNAKER_RUNTIME "Bundle the Spinnaker runtime DLLs into the install tree" OFF)
if (INSTALL_SPINNAKER_RUNTIME)
    install(DIRECTORY "${SPINNAKER_ROOT}/bin64/vs2015/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
    )
endif()
