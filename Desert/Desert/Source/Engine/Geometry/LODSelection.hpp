#pragma once

#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // Coarsest level the automatic pick may return. The draw side clamps again to the submesh's real
    // level count, so "3" only ever means "the coarsest one this mesh actually has".
    inline constexpr int kMaxAutoLOD = 3;

    // THE LOD policy, in one place. MeshRenderer draws with it and the editor's Details panel reports
    // it, so the level a user reads next to a mesh cannot drift from the level actually drawn.
    // forcedLOD >= 0 wins outright; lodBias shifts the automatic pick (+coarser, -finer).
    //
    // Takes the submeshes rather than the Mesh so the policy stays pure CPU code (no GPU buffers) —
    // that is what makes it testable and callable from the editor.
    // THE DISTINCT LEVELS IN A BATCH, ascending. A draw call carries ONE index range and therefore ONE
    // level, so a batch whose members want different levels becomes one draw per level.
    //
    // In the policy header and not inside the renderer because it has three call sites — batched
    // statics, ISM instances, shadow casters — and the rule they share is exactly the one all three
    // were missing. Three inline copies is how one of them ends up disagreeing with the other two.
    inline std::vector<uint32_t> DistinctLODs( const std::vector<uint32_t>& levels )
    {
        std::vector<uint32_t> out;
        for ( const uint32_t level : levels )
            if ( std::find( out.begin(), out.end(), level ) == out.end() )
                out.push_back( level );
        std::sort( out.begin(), out.end() );
        return out;
    }

    // THE COARSEST LEVEL THESE SUBMESHES CAN ACTUALLY DRAW, and it exists to stop an "optimization"
    // that costs draw calls and changes nothing. A mesh with no LOD chain (a primitive, a procedural
    // mesh, anything not imported) draws its base index range whatever level it is asked for — see
    // VulkanRendererAPI::RenderMesh. Splitting an instanced batch of such a mesh by the level the
    // policy computed would turn one draw into four that rasterize identical geometry.
    inline uint32_t MaxAvailableLOD( const std::vector<Submesh>& submeshes )
    {
        std::size_t levels = 1;
        for ( const auto& sm : submeshes )
            levels = std::max( levels, sm.LODs.size() );
        return static_cast<uint32_t>( levels - 1 );
    }

    // THE VIEW A LOD IS ASKED FROM: where the eye is and how it projects. The projection is half the
    // question — the same object at the same distance covers four times the screen through a lens of a
    // quarter of the field of view, and a zoomed-in camera must get the detail it can actually see.
    struct LODView
    {
        glm::vec3 Origin{ 0.0f };
        glm::mat4 Projection{ 1.0f };
    };

    // THE SQUARED SCREEN RADIUS OF A BOUNDING SPHERE, as a fraction of the screen: the one measure every
    // LOD in the engine is chosen by (meshes, ISM instances, landscape tiles).
    // Port of UE ComputeBoundsScreenRadiusSquared (Engine/Source/Runtime/Engine/Private/SceneManagement.cpp):
    // the projection's focal scale × radius / distance. abs() on the focal terms because a Vulkan projection
    // flips Y; |Projection[2][3]| is 1 for a perspective and 0 for an orthographic projection, so an
    // orthographic view measures size alone, as in UE.
    inline float BoundsScreenRadiusSquared( const glm::vec3& center, float radius, const LODView& view )
    {
        const glm::vec3 d        = center - view.Origin;
        const float     distSq   = glm::dot( d, d ) * std::abs( view.Projection[2][3] );
        const float     multiple = std::max( 0.5f * std::abs( view.Projection[0][0] ),
                                             0.5f * std::abs( view.Projection[1][1] ) );
        return ( multiple * radius ) * ( multiple * radius ) / std::max( 1.0f, distSq );
    }

    // THE SCREEN SIZE AT WHICH EACH AUTOMATIC LEVEL BEGINS (UE's per-LOD ScreenSize: the sphere's diameter
    // as a fraction of the screen). Level N is drawn while the object is smaller than kLODScreenSizes[N].
    inline constexpr float kLODScreenSizes[kMaxAutoLOD + 1] = { 1.0f, 0.20f, 0.08f, 0.03f };

    // THE POLICY, taking bounds that were already computed. An Instanced Static Mesh asks this question
    // once per INSTANCE against one shared mesh; walking that mesh's submeshes 49 152 times to rebuild
    // the same box is the difference between per-instance LOD being affordable and not being.
    inline uint32_t SelectLODFromBounds( const glm::mat4& transform, const Common::Math::AABB& localBounds,
                                         const LODView& view, int forcedLOD, int lodBias )
    {
        if ( forcedLOD >= 0 )
            return static_cast<uint32_t>( forcedLOD );

        if ( IsEmpty( localBounds ) )
            return 0; // empty mesh

        // World-space bounding sphere: the box's centre through the transform, radius = half-diagonal × the
        // largest transform scale. Size-aware: a large object keeps full detail farther away than a small one.
        const float scale = glm::max(
             glm::length( glm::vec3( transform[0] ) ),
             glm::max( glm::length( glm::vec3( transform[1] ) ), glm::length( glm::vec3( transform[2] ) ) ) );
        const float     radius = glm::length( localBounds.Max - localBounds.Min ) * 0.5f * scale;
        const glm::vec3 center = glm::vec3( transform * glm::vec4( 0.5f * ( localBounds.Min + localBounds.Max ), 1.0f ) );

        // UE ComputeStaticMeshLOD: the coarsest level whose screen size the object is still below.
        const float screenRadiusSq = BoundsScreenRadiusSquared( center, radius, view );
        int         base           = 0;
        for ( int level = kMaxAutoLOD; level > 0; --level )
        {
            const float half = 0.5f * kLODScreenSizes[level];
            if ( half * half > screenRadiusSq )
            {
                base = level;
                break;
            }
        }
        // Per-mesh bias shifts the auto pick (+coarser / -finer).
        return static_cast<uint32_t>( std::clamp( base + lodBias, 0, kMaxAutoLOD ) );
    }

    // The submesh-taking spelling, for callers that hold a mesh rather than a box. One policy, one
    // definition of a mesh's extent (Geometry::LocalBounds) — the draw side, the editor's Details panel
    // and the culler cannot drift apart about either.
    inline uint32_t SelectLOD( const glm::mat4& transform, const std::vector<Submesh>& submeshes, const LODView& view,
                               int forcedLOD, int lodBias )
    {
        return SelectLODFromBounds( transform, LocalBounds( submeshes ), view, forcedLOD, lodBias );
    }
} // namespace Desert::Geometry
