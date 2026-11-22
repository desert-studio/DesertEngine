#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

namespace Desert::Graphic::RDG
{
    SystemTextures RegisterSystemTextures( Builder& graph, ExternalTexture& black, ExternalTexture& white,
                                           ExternalTexture& blackCube )
    {
        const SystemTextures system{ graph.RegisterExternal( black, "System.Black" ),
                                     graph.RegisterExternal( white, "System.White" ),
                                     graph.RegisterExternal( blackCube, "System.BlackCube" ) };
        // RDG-FAULT1: the images a FaultDefault names, so every graph with system textures can honour one.
        graph.GetFaultDefaults().SetSources( system.Black, system.White, system.BlackCube );
        return system;
    }
} // namespace Desert::Graphic::RDG
