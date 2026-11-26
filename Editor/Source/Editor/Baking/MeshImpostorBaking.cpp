#include "MeshImpostorBaking.hpp"

#include <Editor/Core/HLODApproximateBake.hpp>
#include <Editor/Core/WorldPartitionHLODBuilder.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/HLODImpostor.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/ComponentRegistry.hpp>
#include <Engine/Core/Serialize/EntityPackages.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/StaticMeshComponent.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Core/Logger.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor::Baking
{
    namespace
    {
        struct RunningImpostorBake
        {
            std::unique_ptr<HLODApproximateBake> Bake;
            Common::UUID                         Entity;
            std::string                          Name;      // the entity's tag, for the log
            std::filesystem::path                OutputDir; // beside the mesh asset
        };

        RunningImpostorBake& RunningImpostor()
        {
            static RunningImpostorBake running;
            return running;
        }

        // The bake's one part, mapped from the live component exactly as an HLOD gather maps a saved Static Mesh
        // record (WorldPartitionHLODRules: {Guid, Path} of the mesh, one pair per material slot, the list absent
        // when there are no slots) at one identity instance, so Core::HLODImpostorKey names the same impostor an
        // Impostor HLOD layer would bake for this mesh and these materials.
        Assets::InstancedStaticMeshComponentSer PartOf( const ECS::StaticMeshComponent&  mesh,
                                                        const Reflection::AssetResolver& resolver )
        {
            Assets::InstancedStaticMeshComponentSer part;
            const uint64_t                          meshHandle = static_cast<uint64_t>( mesh.GetMeshHandle() );
            if ( std::string guid = resolver.ToGuid( meshHandle, "StaticMeshAsset" ); !guid.empty() )
                part.MeshGuid = std::move( guid );
            if ( std::string path = resolver.ToPath( meshHandle, "StaticMeshAsset" ); !path.empty() )
                part.MeshPath = std::move( path );
            const auto& slots = mesh.GetMaterialSlots();
            if ( !slots.empty() )
            {
                part.MaterialGuids = std::vector<std::string>{};
                part.MaterialPaths = std::vector<std::string>{};
                for ( const Assets::AssetHandle& slot : slots )
                {
                    const uint64_t handle = static_cast<uint64_t>( slot );
                    part.MaterialGuids->push_back( resolver.ToGuid( handle, "MaterialAsset" ) );
                    part.MaterialPaths->push_back( resolver.ToPath( handle, "MaterialAsset" ) );
                }
            }
            part.InstanceTransforms =
                 std::vector<std::array<float, 16>>{ { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                       1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f } };
            return part;
        }
    } // namespace

    Common::BoolResultStr StartImpostorBakeOf( ::Desert::Core::Scene& scene, const ECS::Entity& entity,
                                               const Assets::AssetManager& assets )
    {
        RunningImpostorBake& running = RunningImpostor();
        if ( running.Bake )
            return Common::MakeError( "Bake Impostor: a bake is already running" );
        if ( !entity.HasComponent<ECS::StaticMeshComponent>() || !entity.HasComponent<ECS::UUIDComponent>() )
            return Common::MakeError( "Bake Impostor: the entity has no static mesh" );
        const std::string name =
             entity.HasComponent<ECS::TagComponent>() ? entity.GetComponent<ECS::TagComponent>().Tag : "Mesh";
        const auto& mesh = entity.GetComponent<ECS::StaticMeshComponent>();
        if ( mesh.GetPrimitive().has_value() || static_cast<uint64_t>( mesh.GetMeshHandle() ) == 0 )
            return Common::MakeError(
                 std::format( "Bake Impostor: '{}' draws no mesh asset to bake beside", name ) );
        const Assets::AssetMetadata* meshFile = assets.FindMetadataByHandle( mesh.GetMeshHandle() );
        if ( meshFile == nullptr || meshFile->Filepath.empty() )
            return Common::MakeError( std::format( "Bake Impostor: '{}''s mesh has no asset file", name ) );
        const auto& baseline = scene.Packages()->BaselinePath();
        if ( !baseline )
            return Common::MakeError( "Bake Impostor: save the scene first (the capture reads the saved world)" );

        auto text = ::Desert::Core::ExternalEntities::ReadSceneFileText( *baseline );
        if ( !text )
            return Common::MakeError( std::format( "Bake Impostor: {}", text.GetError() ) );
        auto loadable = ::Desert::Core::ParseLoadableScene( baseline->string(), text.GetValue() );
        if ( !loadable )
            return Common::MakeError( std::format( "Bake Impostor: {}", loadable.GetError() ) );

        const Reflection::AssetResolver resolver = ::Desert::Core::Serialize::MakeAssetResolver( assets );
        HLODApproximateBakeCell         cell;
        cell.Impostor  = ::Desert::Core::HLODImpostorSettings{};
        cell.Parts     = { PartOf( mesh, resolver ) };
        cell.Key       = ::Desert::Core::HLODImpostorKey( cell.Parts.front(), *cell.Impostor );
        cell.OutputDir = std::filesystem::path( meshFile->Filepath ).parent_path();

        running.OutputDir = cell.OutputDir;
        running.Entity    = entity.GetComponent<ECS::UUIDComponent>().UUID;
        running.Name      = name;
        std::vector<HLODApproximateBakeCell> cells;
        cells.push_back( std::move( cell ) );
        running.Bake = std::make_unique<HLODApproximateBake>( *baseline, loadable.ExtractValue(),
                                                              std::move( cells ), &assets );
        LOG_INFO( "Bake Impostor: '{}' started -> {}", name, running.OutputDir.generic_string() );
        return BOOLSUCCESS;
    }

    void TickImpostorBake( const Assets::AssetManager& assets, ::Desert::Core::Scene* activeScene )
    {
        RunningImpostorBake& running = RunningImpostor();
        if ( !running.Bake || !running.Bake->Tick() )
            return;
        const std::unique_ptr<HLODApproximateBake> bake = std::move( running.Bake );
        if ( bake->Error() )
        {
            LOG_WARN( "Bake Impostor refused: '{}': {}", running.Name, *bake->Error() );
            return;
        }
        if ( bake->Baked().size() != 1 )
        {
            LOG_WARN( "Bake Impostor: '{}' baked {} impostors, not one", running.Name, bake->Baked().size() );
            return;
        }
        auto card = StoreBakedImpostorCard( bake->Baked().front(), running.OutputDir );
        if ( !card )
        {
            LOG_WARN( "Bake Impostor: '{}': {}", running.Name, card.GetError() );
            return;
        }
        const Assets::InstancedStaticMeshComponentSer& stored = card.GetValue();
        if ( activeScene == nullptr )
        {
            LOG_WARN( "Bake Impostor: '{}' was baked but no scene is open to apply it to", running.Name );
            return;
        }
        const auto found = activeScene->FindEntityByID( running.Entity );
        if ( !found || !found->get().HasComponent<ECS::StaticMeshComponent>() )
        {
            LOG_WARN( "Bake Impostor: '{}' was baked but the entity is gone from the active scene", running.Name );
            return;
        }
        const Reflection::AssetResolver resolver = ::Desert::Core::Serialize::MakeAssetResolver( assets );
        const uint64_t                  cardMesh =
             Reflection::ResolveGuidRef( resolver, stored.MeshGuid.value_or( "" ), stored.MeshPath.value_or( "" ),
                                         "StaticMeshAsset", "Bake Impostor card mesh" );
        const bool hasMaterial = stored.MaterialGuids.has_value() && stored.MaterialGuids->size() == 1 &&
                                 stored.MaterialPaths.has_value() && stored.MaterialPaths->size() == 1;
        const uint64_t cardMaterial =
             hasMaterial ? Reflection::ResolveGuidRef( resolver, stored.MaterialGuids->front(),
                                                       stored.MaterialPaths->front(), "MaterialAsset",
                                                       "Bake Impostor card material" )
                         : 0u;
        if ( cardMesh == 0 || cardMaterial == 0 )
        {
            LOG_WARN( "Bake Impostor: '{}': the stored card {} or its material {} did not resolve", running.Name,
                      stored.MeshPath.value_or( "?" ),
                      hasMaterial ? stored.MaterialPaths->front() : std::string( "(none)" ) );
            return;
        }
        const ECS::Entity   target = found->get();
        ECS::StaticMeshDesc desc   = target.GetComponent<ECS::StaticMeshComponent>().GetDesc();
        desc.ImpostorMesh          = Assets::AssetHandle( cardMesh );
        desc.ImpostorMaterial      = Assets::AssetHandle( cardMaterial );
        if ( desc.ImpostorScreenSize <= 0.0f )
            desc.ImpostorScreenSize = kDefaultImpostorScreenSize;
        ECS::SetStaticMesh( *target.GetRegistry(), target.GetHandle(), desc );
        LOG_INFO( "Bake Impostor: '{}' -> card {}, material {}, switch at screen size {}", running.Name,
                  stored.MeshPath.value_or( "" ), stored.MaterialPaths->front(), desc.ImpostorScreenSize );
    }

    bool IsImpostorBakeRunning()
    {
        return RunningImpostor().Bake != nullptr;
    }
} // namespace Desert::Editor::Baking
