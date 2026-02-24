cmake_minimum_required(VERSION 3.21)
project(behaviour_camera LANGUAGES CXX)

# ============================================================================
# TARGET DEFINITION
# ============================================================================
add_executable(behaviour_camera
    src/main.cpp
)

target_compile_features(behaviour_camera PRIVATE cxx_std_20)

# ============================================================================
# DEPENDENCIES (vcpkg-managed)
# ============================================================================
find_package(OpenCV CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

target_link_libraries(behaviour_camera PRIVATE
    opencv_core
    opencv_imgproc
    glfw
    nlohmann_json::nlohmann_json
    OpenGL::GL
)

# ============================================================================
# SPINNAKER SDK (vendor-provided)
# ============================================================================
set(SPINNAKER_ROOT "" CACHE PATH "Root of the Teledyne Spinnaker SDK installation")

if (NOT SPINNAKER_ROOT)
    message(FATAL_ERROR "SPINNAKER_ROOT not set. Set it to e.g. C:/Program Files/Teledyne/Spinnaker")
endif()

add_library(SpinnakerSDK UNKNOWN IMPORTED)

set_target_properties(SpinnakerSDK PROPERTIES
    IMPORTED_LOCATION "${SPINNAKER_ROOT}/lib64/vs2015/Spinnaker_v140.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${SPINNAKER_ROOT}/include"
)

target_link_libraries(behaviour_camera PRIVATE SpinnakerSDK)

# ============================================================================
# INSTALLATION RULES
# ============================================================================
install(TARGETS behaviour_camera RUNTIME DESTINATION bin)

# Copy Spinnaker DLLs
install(DIRECTORY "${SPINNAKER_ROOT}/bin64/vs2015/"
    DESTINATION bin
    FILES_MATCHING PATTERN "*.dll"
)

# Copy vcpkg DLLs
if(EXISTS "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")
    install(DIRECTORY "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
    )
endif()