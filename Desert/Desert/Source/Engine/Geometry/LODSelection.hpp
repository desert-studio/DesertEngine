#pragma once

#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Geometry
{
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
        const glm::vec3 d      = center - view.Origin;
        const float     distSq = glm::dot( d, d ) * std::abs( view.Projection[2][3] );
        const float     multiple =
             std::max( 0.5f * std::abs( view.Projection[0][0] ), 0.5f * std::abs( view.Projection[1][1] ) );
        return ( multiple * radius ) * ( multiple * radius ) / std::max( 1.0f, distSq );
    }

    // THE SCREEN SIZE AT WHICH A LEVEL BEGINS when the mesh's author left it to the engine (UE
    // UStaticMesh::ComputeLODScreenSize, StaticMesh.cpp, the bAutoComputeLODScreenSize rule): a geometric
    // progression below LOD 1's size, never above the previous level's. ScreenSize is UE's measure - the
    // bounding sphere's diameter as a fraction of the screen - and level N is drawn while the object is smaller
    // than ScreenSizes[N]. LOD 0's entry is never compared (nothing is finer); 1 states "the whole screen".
    inline constexpr float kLOD0ScreenSize          = 1.0f;
    inline constexpr float kAutoLOD1ScreenSize      = 0.2f;
    inline constexpr float kAutoLODScreenSizeFactor = 0.4f;
    inline constexpr float kAutoLODScreenSizeGap    = 0.001f; // UE Constants::Tolerance, in our scale

    // THE TRIANGLE DENSITY A LEVEL IS GIVEN UP AT. UE's automatic rule above is blind to the triangle count:
    // a 1.6 M-triangle tree and a 600-triangle flower switch at the same sizes, so the dense mesh keeps tens of
    // triangles per pixel far away (FIELD_Barn: a dandelion's 2 354-triangle LOD 3 drawn 26..66 m away, ~30 px
    // across; an island tree's 800 k-triangle LOD 1 out to 76 m). Here a level also ends where its triangles,
    // spread over the bounding sphere's disc on screen, would get fewer than kAutoLODPixelsPerTriangle pixels
    // each - one 2x2 raster quad: below that the rasterizer shades whole quads per triangle and the vertex and
    // setup cost buys no visible detail. The disc bounds the mesh, so the surface actually drawn keeps MORE
    // triangles per covered pixel than the threshold states, never fewer. A screen size is a fraction of the
    // screen height, so the pixels are counted at a reference height: the sizes are asset data, the same on
    // every display (a taller screen shows a level a little denser than the threshold).
    inline constexpr float kAutoLODPixelsPerTriangle   = 4.0f;
    inline constexpr float kAutoLODReferenceScreenRows = 1080.0f;

    // The screen size below which @p triangles over the bounding sphere's disc get fewer than
    // kAutoLODPixelsPerTriangle reference pixels each: the disc of diameter S x H covers pi/4 (S H)^2 pixels.
    inline float LODDensityScreenSize( const uint32_t triangles )
    {
        constexpr float kPi = 3.14159265358979f;
        return std::sqrt( 4.0f * kAutoLODPixelsPerTriangle * static_cast<float>( triangles ) / kPi ) /
               kAutoLODReferenceScreenRows;
    }

    // Level @p level's automatic size: the progression's, or larger (the finer level given up earlier) when the
    // finer level's @p finerTriangles are already denser than the threshold there; never above @p previous, so
    // a mesh whose finer levels are denser than the threshold even full screen draws them only from inside.
    inline float ComputeLODScreenSize( const uint32_t level, const float previous, const uint32_t finerTriangles )
    {
        if ( level == 0 )
            return kLOD0ScreenSize;
        const float progression =
             kAutoLOD1ScreenSize * std::pow( kAutoLODScreenSizeFactor, static_cast<float>( level - 1 ) );
        return std::clamp( std::max( progression, LODDensityScreenSize( finerTriangles ) ), 0.0f,
                           std::max( 0.0f, previous - kAutoLODScreenSizeGap ) );
    }

    // THE TRIANGLES EACH LEVEL OF THESE SUBMESHES DRAWS, LOD 0 first, one entry per level the mesh has
    // (MaxAvailableLOD + 1): past the end of its own chain a submesh draws its coarsest level, and one with no
    // chain its base range (GrassRenderer's per-level records, VulkanRendererAPI::RenderMesh).
    inline std::vector<uint32_t> LODTriangleCounts( const std::vector<Submesh>& submeshes )
    {
        std::vector<uint32_t> out( MaxAvailableLOD( submeshes ) + 1u, 0u );
        for ( std::size_t level = 0; level < out.size(); ++level )
            for ( const Submesh& sm : submeshes )
                out[level] +=
                     ( sm.LODs.empty() ? sm.IndexCount : sm.LODs[std::min( level, sm.LODs.size() - 1 )].IndexCount ) /
                     3u;
        return out;
    }

    // THE SCREEN SIZE OF EACH LEVEL A MESH DRAWS (UE FStaticMeshRenderData::ScreenSize, resolved after the
    // cache by ResolveSectionInfo): the author's per-LOD values where the mesh's asset states them, the
    // automatic rule for every level it does not (nullopt = UE bAutoComputeLODScreenSize; a list shorter than
    // the chain = UE's "no source model for this LOD" branch). One entry per level the mesh actually has:
    // @p levelTriangles is its LODTriangleCounts.
    inline std::vector<float> ResolveLODScreenSizes( const std::span<const uint32_t>          levelTriangles,
                                                     const std::optional<std::vector<float>>& authored )
    {
        std::vector<float> out;
        out.reserve( levelTriangles.size() );
        for ( std::size_t level = 0; level < levelTriangles.size(); ++level )
        {
            if ( authored && level < authored->size() )
                out.push_back( ( *authored )[level] );
            else
                out.push_back( ComputeLODScreenSize( static_cast<uint32_t>( level ),
                                                     level == 0 ? kLOD0ScreenSize : out.back(),
                                                     level == 0 ? 0u : levelTriangles[level - 1] ) );
        }
        return out;
    }

    // An authored list is a list the selection can walk: one entry per level from LOD 0, each finite and
    // positive, none larger than the level before it (a coarser level that begins at a LARGER size would never
    // be drawn). Returns the defect, or empty.
    inline std::string LODScreenSizesDefect( const std::span<const float> sizes )
    {
        if ( sizes.empty() )
            return "states no level (LOD 0's size comes first)";
        for ( std::size_t level = 0; level < sizes.size(); ++level )
        {
            if ( !std::isfinite( sizes[level] ) || sizes[level] <= 0.0f )
                return std::format( "LOD {}'s screen size is not a finite positive number", level );
            if ( level > 0 && sizes[level] > sizes[level - 1] )
                return std::format( "LOD {}'s screen size is larger than LOD {}'s, so it could never be drawn",
                                    level, level - 1 );
        }
        return {};
    }

    // THE POLICY, taking bounds that were already computed. An Instanced Static Mesh asks this question
    // once per INSTANCE against one shared mesh; walking that mesh's submeshes 49 152 times to rebuild
    // the same box is the difference between per-instance LOD being affordable and not being.
    inline uint32_t SelectLODFromBounds( const glm::mat4& transform, const Common::Math::AABB& localBounds,
                                         const LODView& view, const std::span<const float> screenSizes,
                                         int forcedLOD, int lodBias )
    {
        if ( forcedLOD >= 0 )
            return static_cast<uint32_t>( forcedLOD );
        if ( screenSizes.size() <= 1 )
            return 0; // one level: nothing to choose

        if ( IsEmpty( localBounds ) )
            return 0; // empty mesh

        // World-space bounding sphere: the box's centre through the transform, radius = half-diagonal × the
        // largest transform scale. Size-aware: a large object keeps full detail farther away than a small one.
        const float scale = glm::max(
             glm::length( glm::vec3( transform[0] ) ),
             glm::max( glm::length( glm::vec3( transform[1] ) ), glm::length( glm::vec3( transform[2] ) ) ) );
        const float     radius = glm::length( localBounds.Max - localBounds.Min ) * 0.5f * scale;
        const glm::vec3 center =
             glm::vec3( transform * glm::vec4( 0.5f * ( localBounds.Min + localBounds.Max ), 1.0f ) );

        // UE ComputeStaticMeshLOD: the coarsest level whose screen size the object is still below.
        const int   coarsest       = static_cast<int>( screenSizes.size() ) - 1;
        const float screenRadiusSq = BoundsScreenRadiusSquared( center, radius, view );
        int         base           = 0;
        for ( int level = coarsest; level > 0; --level )
        {
            const float half = 0.5f * screenSizes[static_cast<std::size_t>( level )];
            if ( half * half > screenRadiusSq )
            {
                base = level;
                break;
            }
        }
        // Per-mesh bias shifts the auto pick (+coarser / -finer).
        return static_cast<uint32_t>( std::clamp( base + lodBias, 0, coarsest ) );
    }

    // The submesh-taking spelling, for callers that hold a mesh rather than a box. One policy, one
    // definition of a mesh's extent (Geometry::LocalBounds) — the draw side, the editor's Details panel
    // and the culler cannot drift apart about either.
    inline uint32_t SelectLOD( const glm::mat4& transform, const std::vector<Submesh>& submeshes,
                               const LODView& view, const std::span<const float> screenSizes, int forcedLOD,
                               int lodBias )
    {
        return SelectLODFromBounds( transform, LocalBounds( submeshes ), view, screenSizes, forcedLOD, lodBias );
    }
} // namespace Desert::Geometry
