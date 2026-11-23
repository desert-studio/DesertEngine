#include <Engine/Libraries/EntityLibrary.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/World/Decals/DecalImpactPool.hpp>

namespace Desert::Libraries
{
    Common::BoolResultStr WorldLibrary::SpawnDecal( const World::DecalImpactSpawn& spawn )
    {
        const Core::WorldContext* context = Core::WorldContext::Current();
        if ( context == nullptr || context->World == nullptr )
            return Common::MakeError( "World.spawnDecal: no world runs this call" );
        return context->World->GetDecalImpacts().Spawn( spawn );
    }
} // namespace Desert::Libraries
