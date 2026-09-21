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

# ---------- mdk_bridge GDExtension ----------
add_library(mdkbridge SHARED
    src/mdk_bridge.cpp
    src/arena_presenter.cpp
    src/register_types.cpp)
target_include_directories(mdkbridge PRIVATE "${MDK_CORE_SRC}" src)
target_link_libraries(mdkbridge PRIVATE godot::cpp mdk_core_frontend)
target_compile_options(mdkbridge PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
set_target_properties(mdkbridge PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "mdkbridge"
    LIBRARY_OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_SOURCE_DIR}/../bin/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

# ---------- native (non-Godot) frontend tests ----------
add_executable(mdk_frontend_tests
    "${MDK_REPO_ROOT}/tests/frontend/test_main.cpp")
target_include_directories(mdk_frontend_tests PRIVATE "${MDK_CORE_SRC}" src)
target_link_libraries(mdk_frontend_tests PRIVATE mdk_core_frontend)
target_compile_options(mdk_frontend_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
enable_testing()
add_test(NAME mdk_frontend_tests COMMAND mdk_frontend_tests)
