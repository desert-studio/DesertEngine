// Compiled into every runner that LINKS Desert.lib (ToolsTests, EngineTests, EditorTests) and into no other.
//
// The reflected types register from a static object in the generated Reflection.gen.cpp. Inside Desert.lib
// that object file is only linked when something references it, and the one reference the engine has is
// Renderer.cpp's call at startup. A suite that never touches the renderer (the scene migrator's, the
// settings homes', the world generator's) would therefore see an EMPTY registry -- "the reflected type
// 'SceneSettings' is not registered in this build" -- which before BUILD1 those suites avoided by compiling
// Reflection.gen.cpp themselves. One reference here, in an object the runner always links, gives every
// suite of the runner the registry the editor and the game have.
#include <Engine/Reflection/ReflectionRegistry.hpp>

namespace
{
    [[maybe_unused]] const bool kReflectionLinked = ( Desert::Reflection::ForceLinkGeneratedReflection(), true );
}
