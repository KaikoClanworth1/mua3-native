add_library(nxloader STATIC
    common/common.cpp
    common/profiler.cpp
    crypto/crypto.cpp
    loader/storage.cpp
    loader/formats.cpp
    loader/installer.cpp
)
target_include_directories(nxloader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nxloader PUBLIC fmt::fmt bcrypt)

add_executable(nxextract tools/nxextract.cpp)
target_link_libraries(nxextract PRIVATE nxloader)

add_executable(nsodump tools/nsodump.cpp)
target_link_libraries(nsodump PRIVATE nxloader)

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp core/*.h gpu/*.cpp gpu/*.h)
list(FILTER CORE_SOURCES EXCLUDE REGEX "gpu/d3d11/")
file(GLOB_RECURSE D3D11_SOURCES CONFIGURE_DEPENDS gpu/d3d11/*.cpp gpu/d3d11/*.h)
add_library(nxcore STATIC ${CORE_SOURCES})
target_link_libraries(nxcore PUBLIC nxloader dynarmic)
target_compile_options(nxcore PRIVATE /bigobj)

add_library(platform STATIC platform/display.cpp platform/input.cpp platform/audio.cpp platform/platform.h ${D3D11_SOURCES})
target_link_libraries(platform PUBLIC nxcore imgui SDL3::SDL3-static d3d11 dxgi d3dcompiler xinput xaudio2 winmm)

add_executable(mua3 app/main.cpp)
target_link_libraries(mua3 PRIVATE nxcore platform)

add_executable(MUA3Launcher WIN32 launcher/launcher.cpp)
target_link_libraries(MUA3Launcher PRIVATE nxloader platform windowscodecs)

add_executable(shadertool tools/shadertool.cpp)
target_link_libraries(shadertool PRIVATE nxcore d3dcompiler)

add_executable(stackdump tools/stackdump.cpp)
