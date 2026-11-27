// THE FOLIAGE MODULE'S REGISTRATION WITH THE ENGINE (UE: a plugin's StartupModule): the engine reads foliage types
// and realizes prefab foliage through Core::ModuleHooks, never naming this module; the executable starts it
// (Desert/Launch/LinkedModules.cpp).

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/FoliageTypeAsset.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Runtime/Services/Foliage/FoliageTypeService.hpp>
#include <Engine/World/Foliage/FoliagePrefabs.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace Desert::World::Foliage
{
    void StartupModule()
    {
        Core::ModuleHookLists& hooks = Core::ModuleHooks();
        // A field's prefab instances are entities the systems must see where the field's transforms say they
        // stand this frame (FO-8): realized before the systems tick.
        hooks.PreSystems.push_back( []( Core::Scene& scene ) { RealizePrefabFoliage( scene ); } );
        hooks.FoliageTypes = []( const Common::AssetHandle& handle ) -> const Assets::Serialization::FoliageTypeData*
        { return Runtime::GetFoliageTypeService()->Get( handle ); };
        hooks.Services.push_back(
             { []( const std::weak_ptr<Assets::AssetManager>& assets )
               { Runtime::GetFoliageTypeService()->BindAssetManager( assets ); },
               []() { Runtime::GetFoliageTypeService()->Clear(); } } );
        // A `.defoliage` named by a foliage field or a procedural volume's type list, announced, not read: the
        // asset's constructor adopts the handle its header GUID names, so a type stored BY GUID is on record
        // under HandleForGuid of that GUID (FO-9); FoliageTypeService reads it when a field asks.
        hooks.AssetTypes.push_back(
             { "FoliageTypeAsset",
               []( Assets::AssetManager& m, const std::filesystem::path& full ) -> std::uint64_t
               {
                   auto a = m.FindByPath<Assets::FoliageTypeAsset>( full );
                   if ( !a )
                       a = m.CreateAsset<Assets::FoliageTypeAsset>( full, /*loadAfterCreate=*/false );
                   return a ? static_cast<std::uint64_t>( a->GetMetadata().Handle ) : 0;
               },
               []( const Assets::AssetManager& m, std::uint64_t handle ) -> bool
               { return static_cast<bool>( m.FindByHandle<Assets::FoliageTypeAsset>( Assets::AssetHandle( handle ) ) ); } } );
        // A reachable foliage type keeps the mesh it draws and the prefab it spawns (AssetEviction's walk).
        hooks.AssetReferences.push_back(
             { []( Assets::AssetManager& assets, std::uint64_t handle,
                   const std::function<void( const std::string&, const char* )>& visitGuid )
               {
                   const auto type = assets.ProbeByHandle<Assets::FoliageTypeAsset>( Assets::AssetHandle( handle ) );
                   if ( !type || !type->IsReadyForUse() )
                       return;
                   visitGuid( type->GetData().Mesh.Guid, "a reachable foliage type draws it" );
                   visitGuid( type->GetData().Prefab.Guid, "a reachable foliage type spawns it" );
               } } );
    }
} // namespace Desert::World::Foliage
