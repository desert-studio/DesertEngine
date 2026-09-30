#include "AssetServiceRegistration.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>
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

        // The same rule for materials: registration and the scene-open wait both reach it through here.
        MaterialService* Materials()
        {
            return ResourceRegistry::GetMaterialService();
        }

        // And for textures: registration and the closure's batch read.
        TextureService* Textures()
        {
            return ResourceRegistry::GetTextureService();
        }
    } // namespace

    void EnsureMaterialRegistered( const Assets::Asset<Assets::MaterialAsset>& material )
    {
        auto* service = Materials();
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

    Common::ResultStr<std::string> MaterialTemplateNameOf( const Assets::AssetHandle& material )
    {
        auto* service = Materials();
        if ( service == nullptr )
            return Common::MakeError<std::string>( "there is no material service to walk the parent chain" );
        return Common::MakeSuccess( service->ShaderHandleOf( material ).CompileName );
    }

    Common::ResultStr<Graphic::MaterialOverrides> MaterialOverridesOf( const Assets::AssetHandle& material )
    {
        auto* service = Materials();
        if ( service == nullptr )
            return Common::MakeError<Graphic::MaterialOverrides>( "there is no material service to resolve it" );
        Graphic::MaterialOverrides slots;
        if ( !service->ResolveOverrides( material, slots ) )
            return Common::MakeError<Graphic::MaterialOverrides>( "it resolves to no registered material" );
        return Common::MakeSuccess( std::move( slots ) );
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

    void RequestMeshRead( const Assets::Asset<Assets::MeshAsset>& mesh, Assets::AssetManager& registry )
    {
        EnsureMeshRegistered( mesh, registry );
        // Get answers pending for a cold mesh and starts its worker read; nothing is parsed on this thread.
        if ( auto* service = Meshes(); service != nullptr && mesh )
            (void)service->Get( mesh->GetMetadata().Handle );
    }

    bool DiscoverMesh( const Assets::AssetHandle& handle )
    {
        const auto* service = Meshes();
        return service != nullptr && service->Discover( handle );
    }

    std::vector<Assets::ContentRegistry::ClosureRow> SceneDependencies( const Core::Scene& owner )
    {
        using Common::Content::ContentKind;
        using Common::Content::KindName;
        const auto&                                      scene = owner.GetRegistry();
        std::vector<Assets::ContentRegistry::ClosureRow> roots;
        const auto note = [&roots]( const Assets::AssetHandle& handle, ContentKind kind )
        {
            if ( handle != 0 )
                roots.push_back( { handle, std::string( KindName( kind ) ) } );
        };
        const auto slots = [&note]( const std::vector<Assets::AssetHandle>& materials )
        {
            for ( const auto& material : materials )
                note( material, ContentKind::Material );
        };
        scene.view<const ECS::StaticMeshComponent>().each(
             [&]( const ECS::StaticMeshComponent& mesh )
             {
                 note( mesh.MeshHandle, ContentKind::StaticMesh );
                 slots( mesh.MaterialSlots );
             } );
        scene.view<const ECS::SkinnedMeshComponent>().each(
             [&]( const ECS::SkinnedMeshComponent& mesh )
             {
                 note( mesh.MeshHandle, ContentKind::SkinnedMesh );
                 slots( mesh.MaterialSlots );
             } );
        scene.view<const ECS::InstancedStaticMeshComponent>().each(
             [&]( const ECS::InstancedStaticMeshComponent& mesh )
             {
                 note( mesh.MeshHandle, ContentKind::StaticMesh );
                 slots( mesh.MaterialSlots );
             } );
        scene.view<const ECS::VolumetricCloudComponent>().each(
             [&note]( const ECS::VolumetricCloudComponent& cloud )
             { note( cloud.Data.Material, ContentKind::Material ); } );
        return Assets::ContentRegistry::Closure( roots );
    }

    namespace
    {
        bool IsMeshKind( const std::string& kind )
        {
            using Common::Content::ContentKind;
            using Common::Content::KindName;
            return kind == KindName( ContentKind::StaticMesh ) || kind == KindName( ContentKind::SkinnedMesh );
        }

        // Starts every row's worker read, then waits for each: one batch, no read on this thread.
        std::size_t ReadBatch( const std::vector<Assets::ContentRegistry::ClosureRow>& rows )
        {
            using Common::Content::ContentKind;
            using Common::Content::KindName;
            auto*                            meshes    = Meshes();
            auto*                            materials = Materials();
            auto*                            textures  = Textures();
            std::vector<Assets::AssetHandle> awaited;
            for ( const auto& row : rows )
            {
                if ( IsMeshKind( row.Kind ) && meshes != nullptr )
                    meshes->StartRead( row.Handle, awaited );
                else if ( row.Kind == KindName( ContentKind::Material ) && materials != nullptr )
                    materials->StartRead( row.Handle, awaited );
                else if ( row.Kind == KindName( ContentKind::Texture ) && textures != nullptr )
                    textures->StartRead( row.Handle, awaited );
            }
            std::sort( awaited.begin(), awaited.end() );
            awaited.erase( std::unique( awaited.begin(), awaited.end() ), awaited.end() );
            for ( const auto& read : awaited )
                Assets::AsyncAssetLoader::Get().AwaitOne( read );
            return awaited.size();
        }
    } // namespace

    ClosureResidency AwaitClosure( const std::vector<Assets::ContentRegistry::ClosureRow>& closure )
    {
        using Common::Content::ContentKind;
        using Common::Content::KindName;
        auto*            meshes    = Meshes();
        auto*            materials = Materials();
        ClosureResidency outcome;

        // ALL READS START BEFORE THE FIRST WAIT, so the workers read the closure side by side.
        outcome.Reads = ReadBatch( closure );

        // A MESH WITH NO REGISTRY ROW HAS NO `deps` TO WALK. An imported static mesh lives only in the DDC since
        // AF4h — nothing is ever written at its content path, so no row can describe it — and the materials its
        // envelope names are then known only from the read mesh (the same GUIDs its header states). They and
        // their own closure are the second, and last, batch; for a mesh with a row they are already in the
        // closure and the batch is empty.
        std::vector<Assets::ContentRegistry::ClosureRow> all = closure;
        if ( meshes != nullptr && materials != nullptr )
        {
            std::vector<Assets::ContentRegistry::ClosureRow> roots;
            for ( const auto& row : closure )
                if ( IsMeshKind( row.Kind ) )
                    if ( const auto* asset = meshes->GetAsset( row.Handle ) )
                        for ( const auto& external : asset->GetMaterialHandles() )
                        {
                            const Assets::AssetHandle material = materials->GetAssetHandleByExternal( external );
                            const bool known = std::any_of( all.begin(), all.end(), [&material]( const auto& in )
                                                            { return in.Handle == material; } );
                            if ( material != 0 && !known )
                                roots.push_back( { material, std::string( KindName( ContentKind::Material ) ) } );
                        }
            if ( !roots.empty() )
            {
                const auto more = Assets::ContentRegistry::Closure( roots );
                outcome.Reads += ReadBatch( more );
                all.insert( all.end(), more.begin(), more.end() );
            }
        }
        outcome.Rows = all.size();

        // A rig arrives with its mesh, so the skinned ones may have become buildable only now.
        if ( meshes != nullptr )
            for ( const auto& row : closure )
                if ( IsMeshKind( row.Kind ) )
                    outcome.DrawableMeshes += meshes->Get( row.Handle ) != nullptr ? 1 : 0;
        return outcome;
    }

    ClosureResidency AwaitSceneClosure( const Core::Scene& scene )
    {
        return AwaitClosure( SceneDependencies( scene ) );
    }

    ClosureResidency AwaitAssetClosure( const Assets::AssetHandle& handle, Common::Content::ContentKind kind )
    {
        return AwaitClosure( Assets::ContentRegistry::Closure(
             { { handle, std::string( Common::Content::KindName( kind ) ) } } ) );
    }

    void EnsureTextureRegistered( const Assets::AssetManager& registry, uint64_t handle )
    {
        auto* service = Textures();
        if ( !service || handle == 0 )
            return;
        if ( auto texture = registry.FindByHandle<Assets::TextureAsset>( Common::UUID( handle ) ) )
            service->RegisterAsset( texture );
    }

    Assets::Asset<Assets::SkyboxAsset> RequireSkybox( const Assets::AssetHandle& handle )
    {
        auto* service = ResourceRegistry::GetSkyboxService();
        if ( service == nullptr || static_cast<uint64_t>( handle ) == 0 )
            return nullptr;
        // Declares only (AL1-3): the read is on the loader's worker and the scene's ContentGate waits for it.
        return service->Require( handle );
    }

    Assets::AssetHandle SkyboxHandleAtPath( const std::filesystem::path& path )
    {
        const auto row = Assets::ContentRegistry::RowAtPath( Common::Content::ContentKind::Skybox, path );
        return row ? row->Handle : Assets::AssetHandle( 0 );
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
