#include "ServerCellCollision.hpp"

#include <Engine/Physics/ColliderComponent.hpp>
#include <Engine/Physics/RigidBodyComponent.hpp>
#include <LandscapeCore/Public/LandscapeCollisionShape.hpp>
#include <LandscapeCore/Public/LandscapeTileFiles.hpp>
#include <PhysicsCore/CollisionProfiles.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Json/ReadWrite.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <format>
#include <optional>

namespace Desert::Server
{
    namespace
    {
        namespace Landscape = World::Landscape;

        // The keys of the blocks a server body is built from. A block states only what differs from its
        // component's defaults, so an absent key is the ECS default (taken from the component, not restated).
        struct ColliderRecord
        {
            std::optional<int>       Shape; // Physics::ShapeType, stored as its number
            std::optional<glm::vec3> HalfExtents;
            std::optional<float>     Radius;
            std::optional<float>     HalfHeight;
            std::optional<int>       Axis; // Physics::CapsuleAxis
            std::optional<glm::vec3> Center;
        };
        DESERT_JSON_PARTIAL( ColliderRecord, "an entity record's Collider block (ECS::ColliderData)" );

        struct RigidBodyRecord
        {
            std::optional<int>         Type; // Physics::BodyType
            std::optional<float>       Friction;
            std::optional<float>       Restitution;
            std::optional<std::string> CollisionProfile;
        };
        DESERT_JSON_PARTIAL( RigidBodyRecord, "an entity record's RigidBody block (ECS::RigidBodyData)" );

        struct LandscapeTileRecord
        {
            std::string Landscape; // the root record's id
            int32_t     TileX = 0;
            int32_t     TileZ = 0;
            std::string HeightFile;
        };
        DESERT_JSON_PARTIAL( LandscapeTileRecord, "an entity record's LandscapeTile block (LandscapeTileComponent)" );

        struct LandscapeRootRecord
        {
            std::optional<uint32_t> QuadsPerTile;
            std::optional<float>    SpacingCm;
            std::optional<float>    ZScale;
        };
        DESERT_JSON_PARTIAL( LandscapeRootRecord, "an entity record's Landscape block (LandscapeComponent)" );

        template <typename T>
        std::optional<T> Block( const Assets::EntityData& data, std::string_view key, std::size_t record )
        {
            auto block = data.Components.get( std::string( key ) );
            if ( !block.has_value() )
                return std::nullopt;
            auto read = Common::Json::Read<T>( Common::Json::Write( block.value() ) );
            if ( !read )
            {
                LOG_ERROR( "[Server] record {} (id {}): its {} block does not read: {}", record,
                           data.id ? data.id->ToString() : std::string( "-" ), key, read.GetError() );
                return std::nullopt;
            }
            return read.ExtractValue();
        }

        glm::mat4 LocalMatrix( const Assets::EntityData& data )
        {
            // The editor's Rotation: Euler degrees, composed as the entity's LocalTransform does (glm::quat).
            const glm::vec3 t( data.Translation.value_or( glm::dvec3( 0.0 ) ) );
            const glm::quat r = glm::quat( glm::radians( data.Rotation.value_or( glm::vec3( 0.0f ) ) ) );
            const glm::vec3 s = data.Scale.value_or( glm::vec3( 1.0f ) );
            return glm::translate( glm::mat4( 1.0f ), t ) * glm::mat4_cast( r ) * glm::scale( glm::mat4( 1.0f ), s );
        }

        // The record's WORLD pose (PhysicsECSSystem: ComputeEntityWorldPose), its parents found among the records
        // of its unit - a cell holds a prefab instance's records together.
        glm::mat4 WorldMatrix( const Assets::EntityData& data, std::span<const Assets::EntityData> unitRecords )
        {
            glm::mat4                  world = LocalMatrix( data );
            const Assets::EntityData*  at    = &data;
            for ( std::size_t depth = 0; at->parent && depth < 64; ++depth )
            {
                const auto parent = std::ranges::find_if( unitRecords, [&]( const Assets::EntityData& candidate )
                                                          { return candidate.id && *candidate.id == *at->parent; } );
                if ( parent == unitRecords.end() )
                    break;
                world = LocalMatrix( *parent ) * world;
                at    = &*parent;
            }
            return world;
        }
    } // namespace

    ServerCellCollision::~ServerCellCollision()
    {
        for ( const auto& [record, body] : m_Bodies )
            m_Physics.RemoveBody( body );
    }

    void ServerCellCollision::Add( std::size_t record, const Assets::EntityData& data,
                                   std::span<const Assets::EntityData> unitRecords )
    {
        if ( const auto root = Block<LandscapeRootRecord>( data, "Landscape", record ); root && data.id )
        {
            Landscape::LandscapeRoot frame;
            frame.Origin       = glm::vec3( WorldMatrix( data, unitRecords )[3] );
            frame.QuadsPerTile = root->QuadsPerTile.value_or( frame.QuadsPerTile );
            frame.SpacingCm    = root->SpacingCm.value_or( frame.SpacingCm );
            frame.ZScale       = root->ZScale.value_or( frame.ZScale );
            if ( const auto valid = Landscape::ValidateLandscapeRoot( frame ); !valid )
            {
                LOG_ERROR( "[Server] landscape {}: {}", data.id->ToString(), valid.GetError() );
            }
            else
            {
                const std::string id = data.id->ToString();
                m_Roots[id]          = frame;
                m_RootOfRecord[record] = id;
                std::vector<PendingTile> waiting;
                std::swap( waiting, m_Pending );
                for ( const PendingTile& tile : waiting )
                    if ( tile.Root == id )
                        AddTile( tile, frame );
                    else
                        m_Pending.push_back( tile );
            }
        }

        if ( const auto tile = Block<LandscapeTileRecord>( data, "LandscapeTile", record ) )
        {
            const PendingTile pending{ record, tile->Landscape, tile->TileX, tile->TileZ, tile->HeightFile };
            if ( const auto root = m_Roots.find( tile->Landscape ); root != m_Roots.end() )
                AddTile( pending, root->second );
            else
                m_Pending.push_back( pending ); // the root's unit may come later (it is always loaded, read first)
        }

        const auto collider = Block<ColliderRecord>( data, "Collider", record );
        if ( !collider )
            return;
        const auto rigidBody = Block<RigidBodyRecord>( data, "RigidBody", record );
        if ( !rigidBody )
            return; // the client makes no body of a collider without a rigid body either (PhysicsECSSystem)
        const ECS::RigidBodyData body;
        const auto type = static_cast<Physics::BodyType>( rigidBody->Type.value_or( static_cast<int>( body.Type ) ) );
        if ( type != Physics::BodyType::Static )
            return; // a simulated body is replicated by its authority (NET-ARCH §3), not swept by pawns here
        const ECS::ColliderData shape;
        Physics::BodyDesc       desc;
        desc.Shape = static_cast<Physics::ShapeType>( collider->Shape.value_or( static_cast<int>( shape.Shape ) ) );
        if ( desc.Shape != Physics::ShapeType::Box && desc.Shape != Physics::ShapeType::Sphere &&
             desc.Shape != Physics::ShapeType::Capsule )
        {
            LOG_ERROR( "[Server] record {} (id {}): a mesh / convex hull collider needs its mesh's cooked "
                       "collision, which the server does not read yet - this body is missing on the server",
                       record, data.id ? data.id->ToString() : std::string( "-" ) );
            return;
        }
        desc.HalfExtents = collider->HalfExtents.value_or( shape.HalfExtents );
        desc.Radius      = collider->Radius.value_or( shape.Radius );
        desc.HalfHeight  = collider->HalfHeight.value_or( shape.HalfHeight );
        desc.Axis        = static_cast<Physics::CapsuleAxis>( collider->Axis.value_or( static_cast<int>( shape.Axis ) ) );
        desc.Center      = collider->Center.value_or( shape.Center );
        desc.Type        = type;
        desc.Friction    = rigidBody->Friction.value_or( body.Friction );
        desc.Restitution = rigidBody->Restitution.value_or( body.Restitution );
        const auto profile =
             m_Physics.GetCollisionProfiles().Resolve( rigidBody->CollisionProfile.value_or( body.CollisionProfile ) );
        if ( !profile )
        {
            LOG_ERROR( "[Server] record {}: {}", record, profile.GetError() );
            return;
        }
        desc.Profile = profile.GetValue();
        // The client sizes the shape by the block alone (HalfExtents are not multiplied by the entity's scale),
        // and places it at the world position and rotation.
        const glm::mat4 world = WorldMatrix( data, unitRecords );
        desc.Position         = glm::vec3( world[3] );
        desc.Rotation         = glm::normalize( glm::quat_cast( glm::mat3( glm::normalize( glm::vec3( world[0] ) ),
                                                                         glm::normalize( glm::vec3( world[1] ) ),
                                                                         glm::normalize( glm::vec3( world[2] ) ) ) ) );
        auto made = m_Physics.CreateBody( desc );
        if ( !made )
        {
            LOG_ERROR( "[Server] record {}: no body: {}", record, made.GetError() );
            return;
        }
        Remove( record );
        m_Bodies[record] = made.GetValue();
    }

    void ServerCellCollision::AddTile( const PendingTile& tile, const Landscape::LandscapeRoot& root )
    {
        auto heights = Landscape::ReadLandscapeTileByKey( tile.HeightFile );
        if ( !heights )
        {
            LOG_ERROR( "[Server] landscape tile ({}, {}): {}", tile.TileX, tile.TileZ, heights.GetError() );
            return;
        }
        const Landscape::LandscapeFrame frame = Landscape::LandscapeTileFrame( root, tile.TileX, tile.TileZ );
        Landscape::DecodeLandscapeCollisionHeights( heights.GetValue(), frame.ZScale, m_HeightsCm );
        auto desc = Landscape::LandscapeHeightFieldDesc( heights.GetValue(), frame, m_HeightsCm );
        // UE's landscape preset, as the client's LandscapeCollision.
        const auto profile = m_Physics.GetCollisionProfiles().Resolve( Physics::EngineProfiles::kBlockAll );
        if ( !profile )
        {
            LOG_ERROR( "[Server] landscape tile ({}, {}): {}", tile.TileX, tile.TileZ, profile.GetError() );
            return;
        }
        desc.Profile    = profile.GetValue();
        const auto body = m_Physics.CreateHeightField( desc );
        if ( !body )
        {
            LOG_ERROR( "[Server] landscape tile ({}, {}) has no collision: {}", tile.TileX, tile.TileZ,
                       body.GetError() );
            return;
        }
        Remove( tile.Record );
        m_Bodies[tile.Record]        = body.GetValue();
        m_IsHeightfield[tile.Record] = true;
        ++m_Heightfields;
    }

    void ServerCellCollision::Remove( std::size_t record )
    {
        std::erase_if( m_Pending, [&]( const PendingTile& tile ) { return tile.Record == record; } );
        if ( const auto root = m_RootOfRecord.find( record ); root != m_RootOfRecord.end() )
        {
            m_Roots.erase( root->second );
            m_RootOfRecord.erase( root );
        }
        const auto found = m_Bodies.find( record );
        if ( found == m_Bodies.end() )
            return;
        m_Physics.RemoveBody( found->second );
        m_Bodies.erase( found );
        if ( m_IsHeightfield.erase( record ) != 0 )
            --m_Heightfields;
    }
} // namespace Desert::Server
