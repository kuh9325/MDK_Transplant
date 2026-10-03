cmake_minimum_required(VERSION 3.22)
project(mdk_bridge LANGUAGES CXX)

# Phase 7 (G1) GDExtension build. Standalone by design: mdk_core is
# recompiled from the repo sources (PIC) so this tree stays usable
# without configuring the SDL/native root build first.
#
# godot-cpp is pinned to the spike-validated commit for Godot 4.7.
# Override with -DGODOT_CPP_DIR=/path/to/godot-cpp to reuse an
# existing checkout/build (it must be built with target=template_release,
# precision=single).

set(MDK_GODOT_CPP_COMMIT
    "507ed9d840c01a3c5b2a39af8bb4000bfac30bf5"
    CACHE STRING "godot-cpp commit (Godot 4.7 API)")
set(MDK_GODOT_CPP_SHA256
    "30da4ac295997061a6af81ae531510efd4e6a86e506eb4466edc0898ef3fe048"
    CACHE STRING "SHA256 of the godot-cpp commit tarball")

set(GODOT_CPP_DIR "" CACHE PATH "existing godot-cpp checkout/build root")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(MDK_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(MDK_CORE_SRC "${MDK_REPO_ROOT}/src")

# ---------- godot-cpp ----------
# Spike-validated configuration: Godot 4.7 API, single precision,
# editor-target bindings (Godot.app on macOS is the editor binary).
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version")
set(GODOTCPP_PRECISION "single" CACHE STRING "godot-cpp precision")
set(GODOTCPP_TARGET "editor" CACHE STRING "godot-cpp target")
set(GODOTCPP_ENABLE_TESTING OFF CACHE BOOL "godot-cpp tests" FORCE)

if(GODOT_CPP_DIR)
  add_subdirectory("${GODOT_CPP_DIR}" "${CMAKE_BINARY_DIR}/godot-cpp"
                   EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(
    godot-cpp
    URL "https://github.com/godotengine/godot-cpp/archive/${MDK_GODOT_CPP_COMMIT}.tar.gz"
    URL_HASH "SHA256=${MDK_GODOT_CPP_SHA256}")
  FetchContent_MakeAvailable(godot-cpp)
endif()

# ---------- mdk_core (presentation-authoritative core, PIC) ----------
file(GLOB MDK_CORE_SOURCES CONFIGURE_DEPENDS "${MDK_CORE_SRC}/core/*.cpp")
add_library(mdk_core_frontend STATIC ${MDK_CORE_SOURCES})
target_include_directories(mdk_core_frontend PUBLIC "${MDK_CORE_SRC}")
target_compile_options(mdk_core_frontend PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)

# ---------- optional FFmpeg (Phase 19E — Interplay MVE) ----------
# LGPL 2.1+ dynamically linked dylibs staged by ../fetch_ffmpeg.sh
# into gdextension/third_party/ffmpeg/{include,lib}. Absent the
# staging tree the bridge still builds — the MVE open then fails
# and FUN_0047b674's original missing-file edge runs instead.
set(MDK_FFMPEG_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg"
    CACHE PATH "staged FFmpeg prefix (include/ + lib/)")
set(MDK_WITH_MVE OFF)
if(EXISTS "${MDK_FFMPEG_ROOT}/lib/libavformat.63.dylib" AND
   EXISTS "${MDK_FFMPEG_ROOT}/include/libavformat/avformat.h")
  set(MDK_WITH_MVE ON)
endif()

# ---------- mdk_bridge GDExtension ----------
add_library(mdkbridge SHARED
    src/mdk_bridge.cpp
    src/arena_presenter.cpp
    src/frontend_presenter.cpp
    src/gif_decode.cpp
    src/object_presenter.cpp
    src/register_types.cpp
    src/stream_presenter.cpp
    src/stream_raster.cpp
    src/stream_audio.cpp)
if(MDK_WITH_MVE)
  target_sources(mdkbridge PRIVATE src/mve_player.cpp)
  target_include_directories(mdkbridge PRIVATE "${MDK_FFMPEG_ROOT}/include")
  target_link_libraries(mdkbridge PRIVATE
      "${MDK_FFMPEG_ROOT}/lib/libavformat.63.dylib"
      "${MDK_FFMPEG_ROOT}/lib/libavcodec.63.dylib"
      "${MDK_FFMPEG_ROOT}/lib/libavutil.61.dylib")
  target_compile_definitions(mdkbridge PRIVATE MDK_WITH_MVE=1)
  # @rpath install names resolve next to the dylib in dev
  # (bin/Darwin-arm64/) and in Contents/Frameworks post-export;
  # SKIP_BUILD_RPATH keeps the build-tree absolute path out of
  # the shipped binary's LC_RPATH.
  set_target_properties(mdkbridge PROPERTIES SKIP_BUILD_RPATH TRUE)
  target_link_options(mdkbridge PRIVATE "-Wl,-rpath,@loader_path")
endif()
target_include_directories(mdkbridge PRIVATE "${MDK_CORE_SRC}" src)
target_link_libraries(mdkbridge PRIVATE godot::cpp mdk_core_frontend)
target_compile_options(mdkbridge PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
set_target_properties(mdkbridge PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "mdkbridge"
    LIBRARY_OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_SOURCE_DIR}/../bin/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

# ---------- native (non-Godot) MVE probe ----------
# Phase 19E — verifies MvePlayer against a real MDKBZK.MVE without
# a Godot binary (frame/audio counts, pts, errors, PPM dumps).
if(MDK_WITH_MVE)
  add_executable(mve_probe src/mve_probe.cpp src/mve_player.cpp)
  target_include_directories(mve_probe PRIVATE src
      "${MDK_FFMPEG_ROOT}/include")
  target_link_libraries(mve_probe PRIVATE
      "${MDK_FFMPEG_ROOT}/lib/libavformat.63.dylib"
      "${MDK_FFMPEG_ROOT}/lib/libavcodec.63.dylib"
      "${MDK_FFMPEG_ROOT}/lib/libavutil.61.dylib")
  set_target_properties(mve_probe PROPERTIES SKIP_BUILD_RPATH TRUE)
  target_link_options(mve_probe PRIVATE "-Wl,-rpath,@loader_path")
  set_target_properties(mve_probe PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY
          "${CMAKE_CURRENT_SOURCE_DIR}/../bin/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

# ---------- native (non-Godot) stream census driver ----------
# Phase 19B.2B1 — runs the real mode-5 StreamScene through the SAME
# StreamPresenter/StreamRibbonRaster the GDExtension hosts, so the
# kModelDraw material census (A..G reachability) and representative
# frame shots are measurable without a Godot binary.
add_executable(mdk_stream_census
    src/stream_census.cpp
    src/stream_presenter.cpp
    src/stream_raster.cpp
    src/stream_audio.cpp)
target_include_directories(mdk_stream_census PRIVATE "${MDK_CORE_SRC}" src)
target_link_libraries(mdk_stream_census PRIVATE mdk_core_frontend)
target_compile_options(mdk_stream_census PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)

# ---------- native (non-Godot) frontend tests ----------
add_executable(mdk_frontend_tests
    "${MDK_REPO_ROOT}/tests/frontend/test_main.cpp"
    src/gif_decode.cpp
    src/stream_presenter.cpp
    src/stream_raster.cpp
    src/stream_audio.cpp)
target_include_directories(mdk_frontend_tests PRIVATE "${MDK_CORE_SRC}" src)
target_link_libraries(mdk_frontend_tests PRIVATE mdk_core_frontend)
target_compile_options(mdk_frontend_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
enable_testing()
add_test(NAME mdk_frontend_tests COMMAND mdk_frontend_tests)
