# JNI shared library for Android TTS Service
# Bundles espeak_phonemizer + tgspeechbox + tgsbFrontend into one .so

cmake_minimum_required(VERSION 3.22.1)
project(tgspeechbox_jni C CXX)

# Paths — TGSB_DIR is the repo root (four levels up from src/platforms/android/jni/)
# ESPEAK_NG_DIR is a sibling of the repo root
set(TGSB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." CACHE PATH "Path to TGSpeechBox repo root")
set(ESPEAK_NG_DIR "${TGSB_DIR}/../espeak-ng" CACHE PATH "Path to espeak-ng source")

# Build static libraries
add_subdirectory("${ESPEAK_NG_DIR}/src/ucd-tools" "${CMAKE_CURRENT_BINARY_DIR}/ucd-tools")
add_subdirectory("${TGSB_DIR}/espeak-phonemizer" "${CMAKE_CURRENT_BINARY_DIR}/espeak_phonemizer")
add_subdirectory("${TGSB_DIR}/tgsb-static" "${CMAKE_CURRENT_BINARY_DIR}/tgsb_static")

# Build the JNI shared library
add_library(tgspeechbox_jni SHARED tgsb_jni.cpp)

target_include_directories(tgspeechbox_jni PRIVATE
    "${ESPEAK_NG_DIR}/src/include"
    "${TGSB_DIR}/src"
    "${TGSB_DIR}/src/frontend"
)

target_link_libraries(tgspeechbox_jni PRIVATE
    tgsbFrontend_static
    tgspeechbox_static
    espeak_phonemizer
    ucd
    log     # Android logging
    m       # math
)

# 16 KB page size support (required for Android 15+ devices like Pixel 9)
target_link_options(tgspeechbox_jni PRIVATE "-Wl,-z,max-page-size=16384")

# Strip debug symbols from release builds to minimize .so size
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" OR CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(tgspeechbox_jni PRIVATE -s)
endif()
