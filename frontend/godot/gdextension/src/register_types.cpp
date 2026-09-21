// GDExtension registration — Phase 7 (G1) frontend bridge.
#include "mdk_bridge.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void mdk_bridge_initialize(ModuleInitializationLevel level) {
  if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
    return;
  }
  GDREGISTER_CLASS(MdkBridge);
}

static void mdk_bridge_terminate(ModuleInitializationLevel level) {
  (void)level;
}

extern "C" {

GDExtensionBool GDE_EXPORT mdk_godot_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionConstRefPtr library,
    GDExtensionInitialization* initialization) {
  GDExtensionBinding::InitObject init_obj(
      get_proc_address, const_cast<GDExtensionClassLibraryPtr>(library),
      initialization);
  init_obj.register_initializer(mdk_bridge_initialize);
  init_obj.register_terminator(mdk_bridge_terminate);
  init_obj.set_minimum_library_initialization_level(
      MODULE_INITIALIZATION_LEVEL_SCENE);
  return init_obj.init();
}
}
