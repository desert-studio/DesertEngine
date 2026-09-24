#pragma once

// The landscape as Jolt sees it: one static heightfield body per drawn tile, built from the same uint16
// samples the renderer uploads, placed by the same frame (LandscapeRootOf, LandscapeTileFrame).
//
// WHICH SURFACE. Jolt collides with two planar triangles per cell, split on the (x, z)-(x+1, z+1)
// diagonal; the renderer and SampleLandscapeHeight use the bilinear patch. They agree on every sample and
// along every grid line and differ inside a cell by at most a quarter of its twist |h00 - h10 - h01 + h11|
// (LandscapeRaycast.hpp). A one-surface design — triangles everywhere, shader included — was weighed and
// refused while that bound stays below what shows (an object visibly floating or sinking, > 2 cm, on real
// terrain); the landscape_raycast suite measures it.
//
// SEAMS. Neighbouring tiles share their edge samples, so the two bodies meeting at a seam carry the same
// heights along it: a body crossing it meets one continuous surface, not a step.
//
// EDITS. Each tile's own Physics list of dirty rectangles (LandscapeDirtyConsumer) is patched into the
// existing shape; the body is created once per tile and kept, so a brush stroke never re-creates it.

#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <entt/entt.hpp>

#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    class LandscapeCollision final
    {
    public:
        explicit LandscapeCollision( Physics::PhysicsWorld& world ) : m_World( &world )
        {
        }
        ~LandscapeCollision();

        LandscapeCollision( const LandscapeCollision& )            = delete;
        LandscapeCollision& operator=( const LandscapeCollision& ) = delete;

        /// Removes a tile's body when its entity (or its LandscapeTileComponent) is destroyed. Idempotent.
        void Attach( entt::registry& registry );
        void Detach();

        /**
         * @brief One pass: every tile in @p tiles (DrawableLandscapeTiles — what the renderer draws) has a
         * body carrying its current heights, and no other tile has one.
         *
         * A new tile, or one whose frame moved (its root was moved or re-tiled), gets a fresh body; a tile
         * with pending Physics dirty rectangles is patched in place; a tile unloaded, refused by its root or
         * orphaned loses its body.
         */
        void Sync( std::span<const LandscapeTileRef> tiles );

        /// The body standing for @p entity's tile, or kInvalidBody.
        [[nodiscard]] Physics::BodyHandle BodyOf( entt::entity entity ) const;

    private:
        struct TileBody
        {
            Physics::BodyHandle              Body = Physics::kInvalidBody;
            World::Landscape::LandscapeFrame Frame;
        };

        void OnTileDestroyed( entt::registry& registry, entt::entity entity );
        void Release( entt::entity entity );

        Physics::PhysicsWorld*                     m_World    = nullptr;
        entt::registry*                            m_Registry = nullptr;
        std::unordered_map<entt::entity, TileBody> m_Bodies;
        std::vector<float>                         m_HeightsCm; // reused conversion buffer
    };
} // namespace Desert::ECS
