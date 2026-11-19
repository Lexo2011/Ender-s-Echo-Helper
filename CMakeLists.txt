cmake_minimum_required(VERSION 3.20)
project(EndersEcho VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLL has no VC++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Guild Wars 2 is 64-bit only. Configure with -A x64.")
endif()

# OFF: the version to share. ON: adds the memory diagnostic, Echo hunt, test species and auto-mark.
option(ENDERSECHO_DEV "Build the developer version with research tools" OFF)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/ext/imgui)

set(SOURCES
	src/entry.cpp
	src/ArenaModel.h
	src/ArcAgents.cpp
	src/ArcAgents.h
	src/GameMemory.cpp
	src/GameMemory.h
	src/LiveEcho.cpp
	src/LiveEcho.h
	src/Geometry.h
	src/EchoSource.h
	src/SimulatedEcho.cpp
	src/SimulatedEcho.h
	src/Overlay.cpp
	src/Overlay.h
	src/Settings.cpp
	src/Settings.h
	src/Shared.h
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/imgui_draw.cpp
	${IMGUI_DIR}/imgui_tables.cpp
	${IMGUI_DIR}/imgui_widgets.cpp
)
if(ENDERSECHO_DEV)
	list(APPEND SOURCES
		src/Diagnostics.cpp
		src/Diagnostics.h
		src/EchoHunt.cpp
		src/EchoHunt.h
	)
endif()

add_library(EndersEcho SHARED ${SOURCES})

target_include_directories(EndersEcho PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/ext
	${IMGUI_DIR}
)

target_compile_definitions(EndersEcho PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
	ENDERSECHO_DEV=$<BOOL:${ENDERSECHO_DEV}>)
target_compile_options(EndersEcho PRIVATE /W3 /utf-8)
