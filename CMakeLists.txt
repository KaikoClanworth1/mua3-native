cmake_minimum_required(VERSION 3.24)
# MSVC_DEBUG_INFORMATION_FORMAT: lets the generated recomp library build without debug info
cmake_policy(SET CMP0141 NEW)
project(MUA3Native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if (MSVC)
    add_compile_options(/MP /utf-8 /permissive- /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---- externals -------------------------------------------------------------
set(EXT ${CMAKE_SOURCE_DIR}/external)

# Boost (headers only, used by dynarmic)
add_library(Boost::boost INTERFACE IMPORTED GLOBAL)
set_target_properties(Boost::boost PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${EXT}/boost)
add_library(Boost::headers ALIAS Boost::boost)
set(Boost_FOUND TRUE)
set(Boost_INCLUDE_DIRS ${EXT}/boost)
set(Boost_VERSION 1.86.0)
file(WRITE ${CMAKE_BINARY_DIR}/cmake/FindBoost.cmake "set(Boost_FOUND TRUE)\n")
list(PREPEND CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR}/cmake)

# dynarmic: A64 JIT used as the interim CPU backend
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
add_subdirectory(${EXT}/dynarmic EXCLUDE_FROM_ALL)

# SDL3 (window, gamepad, optional audio)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/SDL EXCLUDE_FROM_ALL)

# Dear ImGui (launcher + debug overlay)
add_library(imgui STATIC
    ${EXT}/imgui/imgui.cpp ${EXT}/imgui/imgui_draw.cpp ${EXT}/imgui/imgui_tables.cpp
    ${EXT}/imgui/imgui_widgets.cpp ${EXT}/imgui/imgui_demo.cpp
    ${EXT}/imgui/backends/imgui_impl_dx11.cpp ${EXT}/imgui/backends/imgui_impl_sdl3.cpp)
target_include_directories(imgui PUBLIC ${EXT}/imgui ${EXT}/imgui/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static d3d11)

# ---- project ---------------------------------------------------------------
add_subdirectory(src)
