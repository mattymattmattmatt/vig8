# Vigilante 8 Arcade (XBLA) - ReXGlue static recompilation
#
# SDK boilerplate lives in generated/rexglue.cmake, which `rexglue codegen`
# writes (Setup runs codegen before the first configure).

cmake_minimum_required(VERSION 3.25)
project(vig8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# The xlive relay client (two files), cloned by Setup into tools/xlive.
set(XLIVE_DIR "${CMAKE_SOURCE_DIR}/../tools/xlive/client" CACHE PATH "xlive client sources")

set(VIG8_SOURCES
    src/main.cpp
    src/stubs.cpp
    src/net.cpp
    src/vr/vr_math.cpp
    src/vr/vr_camera.cpp
    src/vr/vr_runtime.cpp
    src/vr/vr_openxr.cpp
    src/vr/vr_hooks.cpp
    ${XLIVE_DIR}/xlive.cpp)
if(WIN32)
    add_executable(vig8 WIN32 ${VIG8_SOURCES})
else()
    add_executable(vig8 ${VIG8_SOURCES})
endif()
target_include_directories(vig8 PRIVATE ${XLIVE_DIR})

# OpenXR is optional. A flat configure has no loader headers and still builds;
# --vr then logs that Setup.cmd -Vr is required. The headers are the Khronos
# SDK cloned by that setup, or $OPENXR_SDK if it is set.
set(VIG8_HAS_OPENXR 0)
if(WIN32)
    set(_vig8_openxr_header "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/../tools/openxr-sdk/include/openxr/openxr.h")
        set(_vig8_openxr_header "${CMAKE_SOURCE_DIR}/../tools/openxr-sdk/include")
    elseif(DEFINED ENV{OPENXR_SDK} AND EXISTS "$ENV{OPENXR_SDK}/include/openxr/openxr.h")
        set(_vig8_openxr_header "$ENV{OPENXR_SDK}/include")
    endif()
    if(_vig8_openxr_header)
        set(VIG8_HAS_OPENXR 1)
        target_include_directories(vig8 PRIVATE "${_vig8_openxr_header}")
        target_compile_definitions(vig8 PRIVATE XR_USE_GRAPHICS_API_D3D12)
        target_link_libraries(vig8 PRIVATE d3d12 dxgi d3dcompiler dxguid)
        file(GLOB _vig8_openxr_loader
            "${CMAKE_SOURCE_DIR}/../tools/openxr-sdk/build/src/loader/openxr_loader.dll"
            "${CMAKE_SOURCE_DIR}/../tools/openxr-sdk/build/src/loader/*/openxr_loader.dll")
        if(_vig8_openxr_loader)
            list(GET _vig8_openxr_loader 0 _vig8_openxr_loader)
            add_custom_command(TARGET vig8 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_vig8_openxr_loader}" "$<TARGET_FILE_DIR:vig8>")
        endif()
    endif()
endif()
target_compile_definitions(vig8 PRIVATE VIG8_HAS_OPENXR=${VIG8_HAS_OPENXR})

rexglue_setup_target(vig8 GPU_PLUGINS xenos)

target_compile_options(vig8 PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)

# stubs.cpp and net.cpp replace weak generated functions and SDK kernel exports.
if(WIN32)
    target_link_options(vig8 PRIVATE "LINKER:/force:multiple")
    target_link_libraries(vig8 PRIVATE ws2_32)
endif()
