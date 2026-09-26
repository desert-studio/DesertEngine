#include "AssetServiceRegistration.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

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

        // The shell must carry its data before it is keyed: RegisterAsset indexes the material by the
        // EXTERNAL id stored inside the file, and an unparsed shell reports a zero one.
        if ( !material->IsReadyForUse() )
            material->Load();

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
        // now; `Get` parses the cooked file and uploads the buffers, and logs its own reason (with the
        // file in it) when it cannot.
        const Mesh* built = service->Get( handle );
        return ClassifyMeshReadiness( /*registered=*/true, built != nullptr,
                                      built ? built->GetSubmeshes().size() : 0u );
    }
} // namespace Desert::Runtime
