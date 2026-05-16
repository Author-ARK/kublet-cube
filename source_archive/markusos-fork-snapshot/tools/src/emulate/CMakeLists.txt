cmake_minimum_required(VERSION 3.16)
project(kublet-emulate CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# App directory — set by the build script: cmake -DAPP_DIR=.../apps/hello
set(APP_DIR "" CACHE PATH "Absolute path to the app directory")
if(NOT APP_DIR OR NOT EXISTS "${APP_DIR}/src/main.cpp")
  message(FATAL_ERROR "APP_DIR must point to a valid app directory (got: '${APP_DIR}')")
endif()

# Paths
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(KGFX_DIR "${REPO_ROOT}/lib/KGFX")
set(GIFDRAW_DIR "${REPO_ROOT}/lib/GIFDraw")
set(ANIMGIF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/AnimatedGIF")

# Find SDL2 — try pkg-config first, then common brew paths
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 QUIET sdl2)
endif()
if(NOT SDL2_FOUND)
  # Homebrew paths: Apple Silicon and Intel
  list(APPEND CMAKE_PREFIX_PATH
    "/opt/homebrew/opt/sdl2"
    "/usr/local/opt/sdl2"
  )
  find_package(SDL2 REQUIRED)
endif()

# Mock headers take priority over everything
include_directories(BEFORE SYSTEM
  "${CMAKE_CURRENT_SOURCE_DIR}/mock"
)

# Vendored AnimatedGIF (must come after mock but before system includes)
include_directories("${ANIMGIF_DIR}")

# KGFX, GIFDraw, app source headers
include_directories(
  "${KGFX_DIR}"
  "${GIFDRAW_DIR}"
  "${APP_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

# --- Detect which optional libraries the app needs ---
file(READ "${APP_DIR}/src/main.cpp" APP_SOURCE)

# Base C++ sources
set(CPP_SOURCES
  main_wrapper.cpp
  mock/TFT_eSPI_impl.cpp
  mock/Arduino_impl.cpp
  mock/WebServer_impl.cpp
  "${APP_DIR}/src/main.cpp"
  "${KGFX_DIR}/kgfx.cpp"
)

# C-only sources (font files — cannot include C++ headers)
set(C_SOURCES
  "${KGFX_DIR}/font_Arial.c"
  "${KGFX_DIR}/font_ArialBold.c"
)

# AnimatedGIF + GIFDraw (if app uses them)
if(APP_SOURCE MATCHES "#include.*<AnimatedGIF.h>")
  list(APPEND CPP_SOURCES
    "${ANIMGIF_DIR}/AnimatedGIF.cpp"
    "${GIFDRAW_DIR}/GIFDraw.cpp"
  )
  message(STATUS "App uses AnimatedGIF — adding GIF support")
endif()

# TJpg_Decoder (if app uses it)
if(APP_SOURCE MATCHES "#include.*<TJpg_Decoder.h>")
  list(APPEND CPP_SOURCES mock/TJpg_Decoder_impl.cpp)
  message(STATUS "App uses TJpg_Decoder — adding JPEG support")
endif()

add_executable(emulate ${CPP_SOURCES} ${C_SOURCES})

target_include_directories(emulate PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(emulate SDL2::SDL2)

target_compile_definitions(emulate PRIVATE
  EMU_APP_DIR="${APP_DIR}"
)

# Force-include Arduino.h for C++ files (Arduino framework does this automatically)
# Note: C font files must NOT get this
set_source_files_properties(${CPP_SOURCES}
  PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/mock/Arduino.h"
)

# Suppress warnings from app code and KGFX (they're written for ESP32/Arduino)
target_compile_options(emulate PRIVATE
  -Wno-unused-variable
  -Wno-unused-function
  -Wno-sign-compare
  -Wno-missing-field-initializers
  -Wno-deprecated-declarations
)
