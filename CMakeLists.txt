# MDK-Native — macOS native skeleton (Phase 3A)
#
# PROJECT DECISIONS (see docs/ARCHITECTURE_TARGET.md):
#   language  : C++20 (Objective-C++ only at the Metal/platform seam)
#   platform  : SDL3 (Homebrew) for window/input/events
#   GPU       : Metal for presentation of the software framebuffer
#   build     : CMake, out-of-tree (build/ is ignored)
#
# This target contains no original MDK code or data.

cmake_minimum_required(VERSION 3.24)
project(mdk_native LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(SDL3 REQUIRED CONFIG)

# --- core library: platform-neutral, unit-testable, no SDL/Metal headers ----
add_library(mdk_core STATIC
  src/core/bni_directory.cpp
  src/core/bni_image.cpp
  src/core/clock.cpp
  src/core/cmi_directory.cpp
  src/core/collision_query.cpp
  src/core/container.cpp
  src/core/data_root.cpp
  src/core/display_menu.cpp
  src/core/dti_structure.cpp
  src/core/dynamic_objects.cpp
  src/core/file_family.cpp
  src/core/framebuffer.cpp
  src/core/frontend_flow.cpp
  src/core/frontend_menu.cpp
  src/core/frontend_settings.cpp
  src/core/fti_directory.cpp
  src/core/fti_font.cpp
  src/core/fti_sprite.cpp
  src/core/gameplay_input.cpp
  src/core/indexed_image.cpp
  src/core/keyboard_menu.cpp
  src/core/mode_dispatch.cpp
  src/core/mouse_menu.cpp
  src/core/mti_directory.cpp
  src/core/mto_directory.cpp
  src/core/options_menu.cpp
  src/core/player_camera.cpp
  src/core/player_look.cpp
  src/core/player_motion.cpp
  src/core/player_reticle.cpp
  src/core/player_sniper.cpp
  src/core/player_surface.cpp
  src/core/player_vertical.cpp
  src/core/sni_directory.cpp
  src/core/sound_menu.cpp
  src/core/stream_context.cpp
  src/core/traversal_runtime.cpp
  src/core/traversal_script.cpp
  src/core/viewport.cpp
  src/input/input_state.cpp
)
target_include_directories(mdk_core PUBLIC src)
target_compile_options(mdk_core PRIVATE -Wall -Wextra)

# --- native application -----------------------------------------------------
add_executable(mdk_native MACOSX_BUNDLE
  src/app/main.cpp
  src/app/application.cpp
  src/app/diagnostic_scene.cpp
  src/platform/sdl_host.cpp
  src/renderer/metal_presenter.mm
)
target_include_directories(mdk_native PRIVATE src)
target_link_libraries(mdk_native PRIVATE
  mdk_core
  SDL3::SDL3
  "-framework Metal"
  "-framework QuartzCore"
  "-framework Cocoa"
)
target_compile_options(mdk_native PRIVATE -Wall -Wextra)
set_source_files_properties(src/renderer/metal_presenter.mm
  PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

set_target_properties(mdk_native PROPERTIES
  OUTPUT_NAME "mdk-native"
  MACOSX_BUNDLE_BUNDLE_NAME "MDK-Native"
  MACOSX_BUNDLE_GUI_IDENTIFIER "org.mdktransplant.mdk-native"
  MACOSX_BUNDLE_BUNDLE_VERSION "0.3.0"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "0.3.0"
  MACOSX_BUNDLE_INFO_STRING "MDK-Native compatibility reimplementation skeleton (Phase 3A)"
)

# --- developer inspection tool (links the same core parsers as the app) -----
add_executable(mdk_inspect src/tools/mdk_inspect.cpp)
target_link_libraries(mdk_inspect PRIVATE mdk_core)
target_compile_options(mdk_inspect PRIVATE -Wall -Wextra)
set_target_properties(mdk_inspect PROPERTIES OUTPUT_NAME "mdk-inspect")

# --- tests ------------------------------------------------------------------
option(MDK_BUILD_TESTS "Build native unit tests" ON)
if(MDK_BUILD_TESTS)
  enable_testing()
  add_executable(mdk_tests tests/native/test_main.cpp)
  target_link_libraries(mdk_tests PRIVATE mdk_core)
  target_compile_options(mdk_tests PRIVATE -Wall -Wextra)
  add_test(NAME mdk_native_unit COMMAND mdk_tests)
endif()
