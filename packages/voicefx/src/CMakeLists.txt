# VoiceFX native engine. Used by the Flutter plugin (Android) and for the
# desktop unit tests / CLI (cmake -DVOICEFX_BUILD_TESTS=ON).
cmake_minimum_required(VERSION 3.10)
project(voicefx_library VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lame)
file(GLOB LAME_SOURCES ${LAME_DIR}/libmp3lame/*.c)
add_library(mp3lame STATIC ${LAME_SOURCES})
target_include_directories(mp3lame PUBLIC ${LAME_DIR}/include PRIVATE ${LAME_DIR} ${LAME_DIR}/libmp3lame)
target_compile_definitions(mp3lame PRIVATE HAVE_CONFIG_H)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(mp3lame PRIVATE -w -O2)
endif()

set(VOICEFX_SOURCES
  engine/api.cpp
  engine/audio_io.cpp
  engine/effects.cpp
  engine/fft.cpp
  engine/pitch_tracker.cpp
  engine/psola.cpp
  engine/renderer.cpp
  engine/resample.cpp
  engine/tuning.cpp
)

# Shared library loaded by Dart FFI (libvoicefx.so).
add_library(voicefx SHARED ${VOICEFX_SOURCES})
target_include_directories(voicefx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voicefx PRIVATE mp3lame)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(voicefx PRIVATE -O3 -fvisibility=hidden)
endif()
set_target_properties(voicefx PROPERTIES OUTPUT_NAME "voicefx")
target_compile_definitions(voicefx PUBLIC DART_SHARED_LIB)
if(ANDROID)
  # Support Android 15+ 16 KB page sizes.
  target_link_options(voicefx PRIVATE "-Wl,-z,max-page-size=16384")
endif()

option(VOICEFX_BUILD_TESTS "Build desktop tests and CLI" OFF)
if(VOICEFX_BUILD_TESTS)
  add_library(voicefx_core STATIC ${VOICEFX_SOURCES})
  target_include_directories(voicefx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(voicefx_core PRIVATE mp3lame)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(voicefx_core PRIVATE -O3 -Wall -Wextra -fvisibility=hidden)
  endif()
  add_executable(voicefx_tests ../test_native/engine_tests.cpp)
  target_link_libraries(voicefx_tests PRIVATE voicefx_core mp3lame)
  add_executable(voicefx_cli ../test_native/cli.cpp)
  target_link_libraries(voicefx_cli PRIVATE voicefx_core mp3lame)
  enable_testing()
  add_test(NAME engine_tests COMMAND voicefx_tests)
endif()
