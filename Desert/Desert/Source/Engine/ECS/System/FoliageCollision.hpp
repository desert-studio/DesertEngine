#pragma once

// FOLIAGE AS JOLT SEES IT (FOLIAGE-COLLISION). UE gives every instance of a UInstancedStaticMeshComponent an
// FBodyInstance built from the mesh's BodySetup, under the profile its UFoliageType states, and those bodies live
// and unload with the component — in a partitioned world, with the cell's foliage actor.
//
// Ours: ONE static body per foliage field (a field is one type in one World Partition cell, FoliageCells.hpp),
// a compound of the type mesh's one cooked shape placed once per instance (PhysicsWorld::CreateInstancedStaticBody).
// One broadphase entry per cell instead of one per tree; the body follows the field entity: a cell streamed out
// destroys the field and so the body, one streamed in gets one on the next Sync.
//
// WHAT IT READS. The type's Collision (FoliageType.hpp: Enabled, Profile, Shape) through the FoliageTypeService,
// the instances from the InstancedStaticMeshComponent (world matrices), the mesh from the MeshService. A field is
// rebuilt when its instances move (GetInstanceRevision), when its type's Collision changes, or when its mesh does;
// it is never compared instance by instance.

#include <PhysicsCore/PhysicsWorld.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <unordered_map>

namespace Desert::ECS
{
    class FoliageCollision final
    {
    public:
        explicit FoliageCollision( Physics::PhysicsWorld& world ) : m_World( &world )
        {
        }
        ~FoliageCollision();

        FoliageCollision( const FoliageCollision& )            = delete;
        FoliageCollision& operator=( const FoliageCollision& ) = delete;

        /// Removes a field's body when its entity (or its FoliageComponent) is destroyed. Idempotent.
        void Attach( entt::registry& registry );
        void Detach();

        /**
         * @brief One pass: every foliage field whose type collides has a body of its current instances, and no
         * other field has one. A field whose type or mesh is still being read waits for it; one refused (an
         * unknown profile, a mesh Jolt cannot cook) says why once per state and has no body.
         */
        void Sync( entt::registry& registry );

        /// The body standing for @p entity's field, or kInvalidBody.
        [[nodiscard]] Physics::BodyHandle BodyOf( entt::entity entity ) const;

    private:
        struct FieldBody
        {
            Physics::BodyHandle                     Body     = Physics::kInvalidBody;
            uint64_t                                Revision = 0;
            Assets::AssetHandle                     Mesh;
            Assets::Serialization::FoliageCollision Collision;
            bool                                    Refused = false; ///< said once; Body stays invalid
        };

        void OnFieldDestroyed( entt::registry& registry, entt::entity entity );
        void Release( entt::entity entity );

        Physics::PhysicsWorld*                      m_World    = nullptr;
        entt::registry*                             m_Registry = nullptr;
        std::unordered_map<entt::entity, FieldBody> m_Bodies;
    };
} // namespace Desert::ECS
