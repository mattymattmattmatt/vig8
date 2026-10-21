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

set(VIG8_SOURCES src/main.cpp src/stubs.cpp src/net.cpp ${XLIVE_DIR}/xlive.cpp)
if(WIN32)
    add_executable(vig8 WIN32 ${VIG8_SOURCES})
else()
    add_executable(vig8 ${VIG8_SOURCES})
endif()
target_include_directories(vig8 PRIVATE ${XLIVE_DIR})

rexglue_setup_target(vig8 GPU_PLUGINS xenos)

target_compile_options(vig8 PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)

# stubs.cpp and net.cpp replace weak generated functions and SDK kernel exports.
if(WIN32)
    target_link_options(vig8 PRIVATE "LINKER:/force:multiple")
    target_link_libraries(vig8 PRIVATE ws2_32)
endif()
