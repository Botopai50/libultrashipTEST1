cmake_minimum_required(VERSION 3.24)
project(ShadowCaptureTests LANGUAGES CXX)

# The capture's own test, standing on its own.
#
# It used to live in tests/smsr and be built by that directory's CMakeLists, which extracts a dozen pieces
# of the shader to assemble the SMSR kernel. The capture needs none of that: it needs the constant buffer's
# layout and the capture block itself, and nothing else. Tying it to the SMSR harness meant the diagnostic
# tool could not exist without the technique it was written to diagnose -- which is exactly backwards, since
# the tool is what you reach for when the technique is in question.
if(NOT WIN32)
    message(FATAL_ERROR "The shadow capture test uses Direct3D 11 WARP")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(LUS "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(COMMON "${LUS}/include/fast/backends/gfx_direct3d_common.h")
set(BACKEND "${LUS}/src/fast/backends/gfx_direct3d11.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${COMMON}" "${BACKEND}")

# Taken from the real sources rather than copied, so a field added to the constant buffer or a line changed
# in the capture block is compiled here on the next configure instead of drifting silently.
function(extract_between source begin end output)
    string(FIND "${source}" "${begin}" start)
    string(FIND "${source}" "${end}" stop)
    if(start LESS 0 OR stop LESS start)
        message(FATAL_ERROR "Extraction marker missing: ${begin}")
    endif()
    math(EXPR length "${stop} - ${start}")
    string(SUBSTRING "${source}" ${start} ${length} body)
    set(${output} "${body}" PARENT_SCOPE)
endfunction()

file(READ "${COMMON}" common_source)
extract_between("${common_source}" "struct PerShadowCB {" "struct PerDrawCB {" shadow_cb)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shadow_cbuffer.inc" "${shadow_cb}")

file(READ "${BACKEND}" backend_source)
extract_between("${backend_source}" "// SHADOW-CAPTURE-BEGIN" "// SHADOW-CAPTURE-END" capture)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shadow_capture.inc" "${capture}")

find_path(SHADOW_CAPTURE_JSON_INCLUDE nlohmann/json.hpp REQUIRED)
enable_testing()
add_executable(shadow_capture_tests capture_tests.cpp)
target_include_directories(shadow_capture_tests PRIVATE
    "${LUS}/include" "${CMAKE_CURRENT_BINARY_DIR}" "${SHADOW_CAPTURE_JSON_INCLUDE}")
target_compile_definitions(shadow_capture_tests PRIVATE NOMINMAX)
target_link_libraries(shadow_capture_tests PRIVATE d3d11)
add_test(NAME shadow_depth_capture COMMAND shadow_capture_tests)
