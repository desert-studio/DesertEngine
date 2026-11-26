// THE FOLIAGE MODULE'S REGISTRATION WITH THE ENGINE (UE: a plugin's StartupModule): the engine reads foliage types
// and realizes prefab foliage through Core::ModuleHooks, never naming this module; the executable starts it
// (Desert/Launch/LinkedModules.cpp).

#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Foliage/FoliageTypeService.hpp>
#include <Engine/World/Foliage/FoliagePrefabs.hpp>

namespace Desert::World::Foliage
{
    void StartupModule()
    {
        Core::ModuleHookLists& hooks = Core::ModuleHooks();
        // A field's prefab instances are entities the systems must see where the field's transforms say they
        // stand this frame (FO-8): realized before the systems tick.
        hooks.PreSystems.push_back( []( Core::Scene& scene ) { RealizePrefabFoliage( scene ); } );
        hooks.FoliageTypes = []( const Common::AssetHandle& handle ) -> const Assets::Serialization::FoliageTypeData*
        { return Runtime::ResourceRegistry::GetFoliageTypeService()->Get( handle ); };
    }
} // namespace Desert::World::Foliage
