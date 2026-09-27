#include "AssetServiceRegistration.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/ECS/Components.hpp>

#include <Engine/Core/Scene.hpp>

#include <algorithm>
#include <vector>

namespace Desert::Runtime
{
    namespace
    {
        // ONE ACCESSOR PER SERVICE IN THIS FILE, and the suite counts them. Two functions here need the
        // mesh service (register, and register-then-build), and two spellings of "reach the mesh service"
        // in the one file that is allowed to reach it is the first step back towards two implementations.
        MeshService* Meshes()
        {
            return ResourceRegistry::GetMeshService();
        }
    } // namespace

    void EnsureMaterialRegistered( const Assets::Asset<Assets::MaterialAsset>& material )
    {
        auto* service = ResourceRegistry::GetMaterialService();
        if ( !service || !material || service->HasAsset( material->GetMetadata().Handle ) )
            return;

        // REGISTERED UNREAD (AL1-5b). The service keys a material by its external id, and a file-backed
        // `.demat` adopts that id from its header GUID when the shell is constructed (SurfaceMaterialAsset's
        // constructor), so no payload is needed to key it. The read happens on a worker: at scene open
        // through AwaitSceneMaterials, otherwise on the first Get.
        if ( const auto registered = service->RegisterAsset( material ); !registered )
        {
            LOG_ERROR( "[Materials] Material '{}' could not be registered: {}",
                       material->GetMetadata().Filepath.string(), registered.GetError() );
        }
    }

    void EnsureMeshRegistered( const Assets::Asset<Assets::MeshAsset>& mesh, Assets::AssetManager& registry )
    {
        auto* service = Meshes();
        if ( !service || !mesh || service->HasAsset( mesh->GetMetadata().Handle ) )
            return;

        if ( const auto registered = service->RegisterAsset( mesh, registry.weak_from_this() ); !registered )
        {
            LOG_ERROR( "[Mesh] '{}' could not be registered: {}", mesh->GetMetadata().Filepath.string(),
                       registered.GetError() );
        }
    }

    bool DiscoverMesh( const Assets::AssetHandle& handle )
    {
        const auto* service = Meshes();
        return service && service->Discover( handle );
    }

    std::size_t AwaitSceneMeshes( const Core::Scene& owner )
    {
        auto* service = Meshes();
        if ( !service )
            return 0;
        const auto&                      scene = owner.GetRegistry();
        std::vector<Assets::AssetHandle> handles;
        const auto                       note = [&handles]( const Assets::AssetHandle& handle )
        {
            if ( handle )
                handles.push_back( handle );
        };
        scene.view<const ECS::StaticMeshComponent>().each( [&note]( const ECS::StaticMeshComponent& mesh ) { note( mesh.MeshHandle ); } );
        scene.view<const ECS::SkinnedMeshComponent>().each( [&note]( const ECS::SkinnedMeshComponent& mesh ) { note( mesh.MeshHandle ); } );
        scene.view<const ECS::InstancedStaticMeshComponent>().each( [&note]( const ECS::InstancedStaticMeshComponent& mesh ) { note( mesh.MeshHandle ); } );
        std::sort( handles.begin(), handles.end() );
        handles.erase( std::unique( handles.begin(), handles.end() ), handles.end() );
        return service->AwaitResident( handles );
    }

    std::size_t AwaitSceneMaterials( const Core::Scene& owner )
    {
        auto* materials = ResourceRegistry::GetMaterialService();
        if ( !materials )
            return 0;
        const auto*                      meshes = Meshes();
        const auto&                      scene  = owner.GetRegistry();
        std::vector<Assets::AssetHandle> handles;
        // A mesh component with no slots draws with its mesh's own materials (MeshECSSystem fills the slots
        // from them on its first tick), so those are part of the scene's closure too. The meshes were
        // awaited first, so their payloads — and the material ids inside them — are already here.
        const auto note = [&]( const Assets::AssetHandle& mesh, const std::vector<Assets::AssetHandle>& slots )
        {
            handles.insert( handles.end(), slots.begin(), slots.end() );
            if ( !slots.empty() || !mesh || !meshes )
                return;
            if ( const auto* asset = meshes->GetAsset( mesh ) )
                for ( const auto& external : asset->GetMaterialHandles() )
                    handles.push_back( materials->GetAssetHandleByExternal( external ) );
        };
        scene.view<const ECS::StaticMeshComponent>().each( [&note]( const ECS::StaticMeshComponent& mesh ) { note( mesh.MeshHandle, mesh.MaterialSlots ); } );
        scene.view<const ECS::SkinnedMeshComponent>().each( [&note]( const ECS::SkinnedMeshComponent& mesh ) { note( mesh.MeshHandle, mesh.MaterialSlots ); } );
        scene.view<const ECS::InstancedStaticMeshComponent>().each( [&note]( const ECS::InstancedStaticMeshComponent& mesh ) { note( mesh.MeshHandle, mesh.MaterialSlots ); } );
        std::sort( handles.begin(), handles.end() );
        handles.erase( std::unique( handles.begin(), handles.end() ), handles.end() );
        return materials->AwaitResident( handles );
    }

    void EnsureTextureRegistered( const Assets::AssetManager& registry, uint64_t handle )
    {
        auto* service = ResourceRegistry::GetTextureService();
        if ( !service || handle == 0 )
            return;
        if ( auto texture = registry.FindByHandle<Assets::TextureAsset>( Common::UUID( handle ) ) )
            service->RegisterAsset( texture );
    }

    void EnsureSkyboxRegistered( const Assets::Asset<Assets::SkyboxAsset>& skybox )
    {
        auto* service = ResourceRegistry::GetSkyboxService();
        if ( service == nullptr || !skybox || service->Get( skybox->GetMetadata().Handle ) )
            return;

        // Load() is the file-existence check (SkyboxAsset::LoadFromFile) and logs its own refusal by name;
        // registering a skybox whose panorama is gone would bake nothing and say so less clearly.
        if ( !skybox->IsReadyForUse() )
            skybox->Load();

        if ( const auto registered = service->Register( skybox ); !registered )
        {
            LOG_ERROR( "[Skybox] '{}' could not be registered, so the scene has no environment from it: {}",
                       skybox->GetMetadata().Filepath.string(), registered.GetError() );
        }
    }

    MeshReadiness EnsureMeshDrawable( const Assets::Asset<Assets::MeshAsset>& mesh,
                                      Assets::AssetManager&                   registry )
    {
        auto* service = Meshes();
        if ( !service || !mesh )
            return MeshReadiness::NotRegistered;

        EnsureMeshRegistered( mesh, registry );

        const auto handle = mesh->GetMetadata().Handle;
        if ( !service->HasAsset( handle ) )
            return MeshReadiness::NotRegistered;

        // A BUILD, not a probe. Every caller of this function has already decided it needs the geometry
        // now (a thumbnail, an editor command), so it goes through the editor-tool door: read on this
        // thread by AsyncAssetLoader::FlushOne, counted by SyncLoadLedger (plan §2.4(c)).
        const Mesh* built = service->LoadNow( handle );
        return ClassifyMeshReadiness( /*registered=*/true, built != nullptr,
                                      built ? built->GetSubmeshes().size() : 0u );
    }
} // namespace Desert::Runtime
