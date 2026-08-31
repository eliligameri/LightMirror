cmake_minimum_required(VERSION 3.24)
project(LightMirror VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "LightMirror is a native Windows application and must be built on Windows.")
endif()

enable_language(RC)

add_executable(LightMirror WIN32
    src/main.cpp
    src/AudioEngine.cpp
    src/AudioEngine.h
    src/AudioTypes.h
    src/AudioUtils.cpp
    src/AudioUtils.h
    src/DeviceManager.cpp
    src/DeviceManager.h
    src/Settings.cpp
    src/Settings.h
    src/StereoRingBuffer.h
    LightMirror.rc
    LightMirror.manifest
)

target_compile_features(LightMirror PRIVATE cxx_std_17)
target_compile_definitions(LightMirror PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=NTDDI_WIN10_RS2
)

target_link_libraries(LightMirror PRIVATE
    ole32
    uuid
    avrt
    comctl32
)

if(MSVC)
    target_compile_options(LightMirror PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(LightMirror PRIVATE /MANIFEST:NO)
    set_property(TARGET LightMirror PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(LightMirror PROPERTIES
    OUTPUT_NAME "LightMirror"
)

option(LIGHTMIRROR_BUILD_TESTS "Build LightMirror's small audio unit tests" ON)
if(LIGHTMIRROR_BUILD_TESTS)
    enable_testing()
    add_executable(LightMirrorTests
        tests/AudioTests.cpp
        src/AudioUtils.cpp
        src/AudioUtils.h
        src/AudioTypes.h
        src/StereoRingBuffer.h
    )
    target_include_directories(LightMirrorTests PRIVATE src)
    target_compile_features(LightMirrorTests PRIVATE cxx_std_17)
    target_compile_definitions(LightMirrorTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 NTDDI_VERSION=NTDDI_WIN10_RS2
    )
    target_link_libraries(LightMirrorTests PRIVATE ole32 uuid)
    if(MSVC)
        target_compile_options(LightMirrorTests PRIVATE /W4 /permissive- /EHsc /utf-8)
        set_property(TARGET LightMirrorTests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME LightMirrorAudioTests COMMAND LightMirrorTests)
endif()
