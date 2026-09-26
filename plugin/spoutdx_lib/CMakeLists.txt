# Static SpoutDX library for the C4D plugin.
#
# Built separately from the Cinema 4D SDK project so Spout's sources stay outside Maxon's
# warning/stylecheck settings. Spout's own CMake forces the static CRT (/MT) globally, but the
# C4D SDK uses the default dynamic CRT (/MD), so we compile the SpoutDX sources ourselves.

cmake_minimum_required(VERSION 3.20)
project(spoutdx_c4d LANGUAGES CXX)

set(SPOUT_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../external/Spout2/SPOUTSDK")
set(SPOUT_GL "${SPOUT_SDK}/SpoutGL")
set(SPOUT_DX "${SPOUT_SDK}/SpoutDirectX/SpoutDX")

add_library(spoutdx_c4d STATIC
	${SPOUT_DX}/SpoutDX.cpp
	${SPOUT_GL}/SpoutCopy.cpp
	${SPOUT_GL}/SpoutDirectX.cpp
	${SPOUT_GL}/SpoutFrameCount.cpp
	${SPOUT_GL}/SpoutSenderNames.cpp
	${SPOUT_GL}/SpoutSharedMemory.cpp
	${SPOUT_GL}/SpoutUtils.cpp
)

target_compile_definitions(spoutdx_c4d PRIVATE SPOUT_BUILD_STATIC _CRT_SECURE_NO_WARNINGS)
target_include_directories(spoutdx_c4d PUBLIC ${SPOUT_DX} ${SPOUT_GL})
set_target_properties(spoutdx_c4d PROPERTIES
	CXX_STANDARD 17
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Test receiver: grabs one frame from a sender and writes a BMP.
add_executable(spout_grab ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/spout_grab/spout_grab.cpp)
target_link_libraries(spout_grab PRIVATE spoutdx_c4d d3d11 dxgi)
set_target_properties(spout_grab PROPERTIES
	CXX_STANDARD 17
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
