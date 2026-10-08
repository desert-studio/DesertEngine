#include <Engine/ECS/System/FracturePieceDraw.hpp>

#include <Engine/Destruction/FractureEdit.hpp>
#include <Engine/Destruction/FracturePieces.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/DestructibleComponent.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/FracturePreviewComponent.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Runtime/SelectionContext.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>

#include <format>
#include <span>

namespace Desert::ECS
{
    namespace
    {
        glm::mat4 WorldOf( entt::registry& registry, entt::entity entity )
        {
            glm::mat4    world   = registry.get<TransformComponent>( entity ).GetTransform();
            entt::entity current = entity;
            while ( registry.has<RelationshipComponent>( current ) )
            {
                const auto& rel = registry.get<RelationshipComponent>( current );
                if ( rel.Parent == entt::null )
                    break;
                current = rel.Parent;
                if ( registry.has<TransformComponent>( current ) )
                    world = registry.get<TransformComponent>( current ).GetTransform() * world;
            }
            return world;
        }
    } // namespace

    const FracturePieceDraw::FractureMeshes*
    FracturePieceDraw::Meshes( const std::shared_ptr<const Destruction::FractureData>& fracture )
    {
        auto found = m_Meshes.find( fracture.get() );
        if ( found != m_Meshes.end() && found->second.Data.lock() == fracture )
            return &found->second;

        FractureMeshes meshes;
        meshes.Data = fracture;
        for ( const int32_t node : Destruction::DrawnPieces( fracture->Nodes ) )
        {
            const std::string owner =
                 std::format( "fracture {} piece {}", Common::Content::AssetGuidToText( fracture->Guid ), node );
            auto core =
                 Geometry::DynamicMeshFromSerialized( fracture->Nodes[static_cast<size_t>( node )].Mesh, owner );
            if ( !core.IsSuccess() )
            {
                LOG_ERROR( "[Destruction] {} is not drawn: {}", owner, core.GetError() );
                continue;
            }
            auto render = Geometry::ToRenderMesh( core.GetValue() );
            if ( !render.IsSuccess() )
            {
                LOG_ERROR( "[Destruction] {} is not drawn: {}", owner, render.GetError() );
                continue;
            }
            const Geometry::RenderMeshData data = render.ExtractValue();
            auto mesh = std::make_shared<DynamicMesh>( data.Vertices, data.Indices, data.Submeshes );
            if ( auto uploaded = mesh->Invalidate(); !uploaded.IsSuccess() )
            {
                LOG_ERROR( "[Destruction] {} is not drawn: {}", owner, uploaded.GetError() );
                continue;
            }
            meshes.ByNode[node] = meshes.Pieces.size();
            meshes.Pieces.push_back( { node, std::move( mesh ), data.SubmeshMaterialIds } );
        }
        m_Meshes[fracture.get()] = std::move( meshes );
        return &m_Meshes[fracture.get()];
    }

    void FracturePieceDraw::Bind( EntityPieces& entity, const FractureMeshes& meshes,
                                  const Destruction::FractureData& fracture,
                                  std::vector<Assets::AssetHandle> sourceSlots, uint32_t materialsVersion,
                                  const Services& services )
    {
        entity.Fracture         = &fracture;
        entity.MaterialsVersion = materialsVersion;
        entity.SourceSlots      = std::move( sourceSlots );
        entity.Owned.clear();
        entity.Bindings.clear();

        std::vector<Graphic::MaterialInstancePtr> source;
        for ( const auto& slot : entity.SourceSlots )
        {
            auto inst = services.Instance( slot );
            source.push_back( inst ? std::move( inst ) : services.DefaultInstance() );
        }
        if ( source.empty() )
            source.push_back( services.DefaultInstance() );

        const auto interiorChoice = Destruction::ChooseInteriorMaterial(
             fracture, Common::Content::AssetGuidToText( fracture.Guid ),
             [&]( const Common::Content::AssetGuid& guid ) { return !services.MaterialByGuid( guid ).IsNull(); } );
        Graphic::MaterialInstancePtr interior;
        if ( interiorChoice.Error.empty() )
            interior = services.Instance( services.MaterialByGuid( interiorChoice.Material ) );
        if ( !interior )
        {
            if ( m_Reported.insert( &fracture ).second )
                LOG_ERROR( "[Destruction] {}",
                           interiorChoice.Error.empty()
                                ? std::format( "fracture '{}': its interior material could not "
                                               "be instanced; the default surface draws it",
                                               Common::Content::AssetGuidToText( fracture.Guid ) )
                                : interiorChoice.Error );
            interior = services.DefaultInstance();
        }

        entity.Owned = source;
        entity.Owned.push_back( interior );
        for ( const PieceMesh& piece : meshes.Pieces )
        {
            auto binding = std::make_shared<Graphic::MaterialSlotBinding>();
            for ( const auto& choice : Destruction::PieceSubmeshMaterials(
                       piece.SubmeshMaterialIds, fracture.InteriorMaterialId, source.size() ) )
            {
                const auto& inst = choice.Interior ? interior : source[choice.SourceSlot];
                binding->Owned.push_back( inst );
                binding->Slots.push_back( inst.get() );
            }
            entity.Bindings.push_back( std::move( binding ) );
        }
    }

    std::unordered_set<entt::entity> FracturePieceDraw::Record( entt::registry&                       registry,
                                                                Graphic::Render::RenderCommandBuffer& commands,
                                                                uint32_t        materialsVersion,
                                                                const Services& services )
    {
        std::unordered_set<entt::entity> drawn;
        auto record = [&]( entt::entity entity, const std::shared_ptr<const Destruction::FractureData>& fracture,
                           std::span<const std::optional<glm::mat4>> simulated,
                           std::span<const glm::dvec3>               offsets )
        {
            if ( !fracture || ECS::IsHidden( registry, entity ) )
                return;
            const FractureMeshes* meshes = Meshes( fracture );

            // The source materials: the entity's own slots when it is the static mesh being fractured (its
            // overrides are what the level shows), else the source mesh asset's.
            std::vector<Assets::AssetHandle> slots;
            if ( registry.has<StaticMeshComponent>( entity ) )
                slots = registry.get<StaticMeshComponent>( entity ).MaterialSlots;
            if ( slots.empty() )
                slots = services.SourceSlots( Assets::AssetHandle(
                     static_cast<uint64_t>( Common::Content::HandleForGuid( fracture->SourceMesh ) ) ) );

            EntityPieces& pieces = m_Entities[entity];
            if ( pieces.Fracture != fracture.get() || pieces.MaterialsVersion != materialsVersion ||
                 pieces.SourceSlots != slots || pieces.Bindings.size() != meshes->Pieces.size() )
                Bind( pieces, *meshes, *fracture, std::move( slots ), materialsVersion, services );

            const bool outlined =
                 registry.has<UUIDComponent>( entity ) &&
                 Runtime::SelectionContext::Contains( registry.get<UUIDComponent>( entity ).UUID );
            for ( const auto& instance :
                  Destruction::PieceInstances( fracture->Nodes, WorldOf( registry, entity ), simulated, offsets ) )
            {
                const auto at = meshes->ByNode.find( instance.Node );
                if ( at == meshes->ByNode.end() )
                    continue; // logged when its mesh failed to build
                commands.Emplace<Graphic::Render::DrawStaticMeshCommand>(
                     static_cast<uint32_t>( entity ), meshes->Pieces[at->second].Mesh.get(),
                     pieces.Bindings[at->second], instance.Transform, outlined, /*hidden*/ 0ull, /*forcedLOD*/ -1,
                     /*lodBias*/ 0,
                     /*castShadows*/ true, /*receiveShadows*/ true, /*sortPriority*/ 0 );
            }
            drawn.insert( entity );
        };

        // The preview first: an entity in the Fracture Mode shows the tool's pieces, exploded.
        for ( const auto entity : registry.view<FracturePreviewComponent, TransformComponent>() )
        {
            const auto& preview = registry.get<FracturePreviewComponent>( entity );
            if ( !preview.Fracture )
                continue;
            const auto offsets = Destruction::ExplodedOffsets( preview.Fracture->Nodes, preview.View );
            record( entity, preview.Fracture, {}, offsets );
        }
        for ( const auto entity : registry.view<DestructibleComponent, TransformComponent>() )
        {
            if ( drawn.contains( entity ) )
                continue;
            const auto& destructible = registry.get<DestructibleComponent>( entity );
            if ( destructible.Data.Fracture.IsNull() )
                continue;
            record( entity, services.Fracture( destructible.Data.Fracture ), destructible.RuntimeNodeWorld, {} );
        }

        // Forget entities and fractures nothing draws any more.
        std::erase_if( m_Entities, [&]( const auto& entry ) { return !drawn.contains( entry.first ); } );
        std::erase_if( m_Meshes, []( const auto& entry ) { return entry.second.Data.expired(); } );
        return drawn;
    }
} // namespace Desert::ECS
