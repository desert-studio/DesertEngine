#include <Engine/ECS/System/FoliageCollision.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/ColliderMesh.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Foliage/FoliageTypeService.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <vector>
#include <vector>

namespace Desert::ECS
{
    FoliageCollision::~FoliageCollision()
    {
        Detach();
        for ( const auto& [entity, field] : m_Bodies )
            m_World->RemoveBody( field.Body );
    }

    void FoliageCollision::Attach( entt::registry& registry )
    {
        if ( m_Registry == &registry )
            return;
        Detach();
        registry.on_destroy<FoliageComponent>().connect<&FoliageCollision::OnFieldDestroyed>( this );
        m_Registry = &registry;
    }

    void FoliageCollision::Detach()
    {
        if ( m_Registry == nullptr )
            return;
        m_Registry->on_destroy<FoliageComponent>().disconnect( this );
        m_Registry = nullptr;
    }

    void FoliageCollision::OnFieldDestroyed( entt::registry&, entt::entity entity )
    {
        Release( entity );
    }

    void FoliageCollision::Release( entt::entity entity )
    {
        const auto found = m_Bodies.find( entity );
        if ( found == m_Bodies.end() )
            return;
        m_World->RemoveBody( found->second.Body );
        m_Bodies.erase( found );
    }

    Physics::BodyHandle FoliageCollision::BodyOf( entt::entity entity ) const
    {
        const auto found = m_Bodies.find( entity );
        return found == m_Bodies.end() ? Physics::kInvalidBody : found->second.Body;
    }

    void FoliageCollision::Sync( entt::registry& registry )
    {
        auto* types  = Runtime::ResourceRegistry::GetFoliageTypeService();
        auto* meshes = Runtime::ResourceRegistry::GetMeshService();

        std::vector<entt::entity>&       live   = m_Live;
        live.clear();
        auto                             fields = registry.view<FoliageComponent, InstancedStaticMeshComponent>();
        for ( const entt::entity entity : fields )
        {
            const auto& foliage = fields.get<FoliageComponent>( entity );
            const auto& ism     = fields.get<InstancedStaticMeshComponent>( entity );

            const Assets::Serialization::FoliageTypeData* type = types ? types->Get( foliage.FoliageType ) : nullptr;
            if ( type == nullptr )
            {
                // Still being read (or failed, said by the service): keep what stands until the type is known.
                if ( m_Bodies.contains( entity ) )
                    live.push_back( entity );
                continue;
            }
            if ( type->IsPrefab() || !type->Collision.Enabled || ism.GetInstanceTransforms().empty() ||
                 ism.GetMeshHandle().IsNull() )
                continue; // no body: swept below

            live.push_back( entity );
            if ( const auto found = m_Bodies.find( entity );
                 found != m_Bodies.end() && found->second.Revision == ism.GetInstanceRevision() &&
                 found->second.Mesh == ism.GetMeshHandle() && found->second.Collision == type->Collision )
                continue; // built (or refused) for exactly this state

            const Assets::MeshAsset* asset = meshes ? meshes->GetAsset( ism.GetMeshHandle() ) : nullptr;
            if ( asset == nullptr )
                continue; // the mesh is still loading: the old body (if any) stands until the new one is built

            FieldBody field;
            field.Revision  = ism.GetInstanceRevision();
            field.Mesh      = ism.GetMeshHandle();
            field.Collision = type->Collision;

            const auto refuse = [&]( const std::string& why )
            {
                LOG_ERROR( "[Foliage] the field of type '{}' ({} instances) does not collide: {}",
                           type->Mesh.Path, ism.GetInstanceTransforms().size(), why );
                field.Refused = true;
            };

            const auto* staticAsset = dynamic_cast<const Assets::StaticMeshAsset*>( asset );
            auto        profile     = m_World->GetCollisionProfiles().Resolve( type->Collision.Profile );
            if ( staticAsset == nullptr )
                refuse( "its mesh is skinned; foliage collides with a static mesh" );
            else if ( !profile )
                refuse( profile.GetError() );
            else
            {
                const ColliderMesh source =
                     BuildColliderMesh( staticAsset->GetVertices(), staticAsset->GetIndices(), glm::vec3( 1.0f ) );
                Physics::InstancedStaticBodyDesc desc;
                desc.Shape       = type->Collision.Shape == Assets::Serialization::FoliageCollisionShape::Mesh
                                        ? Physics::ShapeType::Mesh
                                        : Physics::ShapeType::ConvexHull;
                desc.MeshPoints  = source.Points;
                desc.MeshIndices = source.Indices;
                desc.Instances   = ism.GetInstanceTransforms();
                desc.Profile     = profile.GetValue();
                auto created     = m_World->CreateInstancedStaticBody( desc );
                if ( created )
                    field.Body = created.GetValue();
                else
                    refuse( created.GetError() );
            }

            Release( entity );
            m_Bodies.emplace( entity, field );
        }

        std::sort( live.begin(), live.end() );
        // A field that left the view (its type stopped colliding, its instances went) loses its body.
        for ( auto it = m_Bodies.begin(); it != m_Bodies.end(); )
        {
            if ( std::binary_search( live.begin(), live.end(), it->first ) )
            {
                ++it;
                continue;
            }
            m_World->RemoveBody( it->second.Body );
            it = m_Bodies.erase( it );
        }
    }
} // namespace Desert::ECS
