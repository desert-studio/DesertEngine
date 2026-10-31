#include <Engine/Libraries/DeformationLibrary.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>

namespace Desert::Libraries
{
    Common::BoolResultStr DeformationLibrary::Deform( const DeformCall& call )
    {
        const Core::WorldContext* context = Core::WorldContext::Current();
        if ( context == nullptr || context->World == nullptr )
            return Common::MakeError( "World.deform: no world runs this call" );
        return context->World->GetDeformation().Submit(
             World::DeformContact{ .Stamp = call.Stamp, .At = call.At, .Params = call.Params } );
    }
} // namespace Desert::Libraries
