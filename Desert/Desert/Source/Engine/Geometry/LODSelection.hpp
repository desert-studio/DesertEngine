#pragma once

#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <Common/Core/Math/Simd.hpp>

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
    // Into @p out (cleared, its capacity kept): the per-frame callers (the batch loops of every pass) pass a
    // scratch vector, so a steady frame allocates nothing per bucket (UE FMemStack: no heap per batch).
    inline void DistinctLODs( const std::vector<uint32_t>& levels, std::vector<uint32_t>& out )
    {
        out.clear();
        for ( const uint32_t level : levels )
            if ( std::find( out.begin(), out.end(), level ) == out.end() )
                out.push_back( level );
        std::sort( out.begin(), out.end() );
    }
    inline std::vector<uint32_t> DistinctLODs( const std::vector<uint32_t>& levels )
    {
        std::vector<uint32_t> out;
        DistinctLODs( levels, out );
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
    // spread over the pixels the mesh can cover on screen, would get fewer than kAutoLODPixelsPerTriangle pixels
    // each - one 2x2 raster quad: below that the rasterizer shades whole quads per triangle and the vertex and
    // setup cost buys no visible detail. What the mesh can cover is the bounding sphere's disc, or its surface
    // seen face-on where that is smaller (LODScreenCoverage): a mesh of cards - a grass clump, a flower, a leaf
    // crown, a fence - covers a third of its disc or less, and counted against the whole disc its levels kept
    // three triangles and more per covered pixel (GRASS-LOD-1: FIELD_Barn's dandelions, 23.5 k triangles at
    // LOD 0, 4.5 ms of G-buffer and masked prepass). Overlap only lowers what is covered, so the surface actually
    // drawn keeps MORE triangles per covered pixel than the threshold states, never fewer. A screen size is a
    // fraction of the screen height, so the pixels are counted at a reference height: the sizes are asset data,
    // the same on every display (a taller screen shows a level a little denser than the threshold).
    inline constexpr float kAutoLODPixelsPerTriangle   = 4.0f;
    inline constexpr float kAutoLODReferenceScreenRows = 1080.0f;

    // THE SHARE OF ITS BOUNDING SPHERE'S DISC A MESH CAN COVER ON SCREEN, in (0, 1]: the area of its LOD 0
    // triangles - every one facing the view and none hiding another, more than any view can show - over the disc
    // of the sphere every LOD measure here uses (the half diagonal of LocalBounds), at most 1. A compact solid's
    // surface exceeds its disc (a rock, a crate, a house stay at 1); a mesh of open cards, or a long thin part (a
    // pole: pi x its width x its length against its length's disc), covers less - and the area is still at least
    // what any view of it covers. One value per mesh, measured where its CPU triangles are (StaticMesh,
    // DynamicMesh: submesh-local indices from VertexOffset); 1 for a mesh without triangles or bounds.
    [[nodiscard]] inline float LODScreenCoverage( const std::vector<Vertex>&  vertices,
                                                  const std::vector<Index>&   indices,
                                                  const std::vector<Submesh>& submeshes )
    {
        const Common::Math::AABB bounds = LocalBounds( submeshes );
        if ( submeshes.empty() || IsEmpty( bounds ) )
            return 1.0f;
        const glm::vec3 diagonal = bounds.Max - bounds.Min;
        double          area     = 0.0;
        for ( const Submesh& sm : submeshes )
        {
            const std::size_t first = sm.IndexOffset / 3u;
            const std::size_t end   = std::min<std::size_t>( indices.size(), first + sm.IndexCount / 3u );
            for ( std::size_t t = first; t < end; ++t )
            {
                const std::size_t a = std::size_t{ sm.VertexOffset } + indices[t].V1;
                const std::size_t b = std::size_t{ sm.VertexOffset } + indices[t].V2;
                const std::size_t c = std::size_t{ sm.VertexOffset } + indices[t].V3;
                if ( a >= vertices.size() || b >= vertices.size() || c >= vertices.size() )
                    continue;
                const glm::vec3 ab = vertices[b].Position - vertices[a].Position;
                const glm::vec3 ac = vertices[c].Position - vertices[a].Position;
                area += 0.5 * static_cast<double>( glm::length( glm::cross( ab, ac ) ) );
            }
        }
        constexpr double kPi  = 3.14159265358979;
        const double     disc = 0.25 * kPi * static_cast<double>( glm::dot( diagonal, diagonal ) );
        if ( !( disc > 0.0 ) || !( area > 0.0 ) )
            return 1.0f;
        return static_cast<float>( std::min( 1.0, area / disc ) );
    }

    // The screen size below which @p triangles over the pixels the mesh covers get fewer than
    // kAutoLODPixelsPerTriangle reference pixels each: the disc of diameter S x H covers pi/4 (S H)^2 pixels, the
    // mesh @p coverage of them (LODScreenCoverage).
    inline float LODDensityScreenSize( const uint32_t triangles, const float coverage )
    {
        constexpr float kPi = 3.14159265358979f;
        return std::sqrt( 4.0f * kAutoLODPixelsPerTriangle * static_cast<float>( triangles ) /
                          ( kPi * std::clamp( coverage, 1.0e-4f, 1.0f ) ) ) /
               kAutoLODReferenceScreenRows;
    }

    // Level @p level's automatic size: the progression's, or larger (the finer level given up earlier) when the
    // finer level's @p finerTriangles are already denser than the threshold there over the mesh's @p coverage of
    // its disc; never above @p previous, so a mesh whose finer levels are denser than the threshold even full
    // screen draws them only from inside.
    inline float ComputeLODScreenSize( const uint32_t level, const float previous, const uint32_t finerTriangles,
                                       const float coverage )
    {
        if ( level == 0 )
            return kLOD0ScreenSize;
        const float progression =
             kAutoLOD1ScreenSize * std::pow( kAutoLODScreenSizeFactor, static_cast<float>( level - 1 ) );
        const float density = LODDensityScreenSize( finerTriangles, coverage );
        return std::clamp( std::max( progression, density ), 0.0f,
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
    // @p levelTriangles is its LODTriangleCounts, @p coverage its LODScreenCoverage.
    inline std::vector<float> ResolveLODScreenSizes( const std::span<const uint32_t>          levelTriangles,
                                                     const std::optional<std::vector<float>>& authored,
                                                     const float                              coverage )
    {
        std::vector<float> out;
        out.reserve( levelTriangles.size() );
        for ( std::size_t level = 0; level < levelTriangles.size(); ++level )
        {
            if ( authored && level < authored->size() )
                out.push_back( ( *authored )[level] );
            else
            {
                // Locals, not conditionals in the call's by-value arguments (MSVC hangs on that form).
                const float    previous = level == 0 ? kLOD0ScreenSize : out.back();
                const uint32_t finer    = level == 0 ? 0u : levelTriangles[level - 1];
                out.push_back( ComputeLODScreenSize( static_cast<uint32_t>( level ), previous, finer, coverage ) );
            }
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

    // THE POLICY FOR A RUN OF INSTANCES OF ONE MESH, eight per register (Common/Core/Math/Simd.hpp Float8; UE
    // ComputeBoundsScreenSize over a batch): @p levels[i] = the level of the instance at @p transforms[i], for
    // i < min( sizes ). An Instanced Static Mesh asks this once per INSTANCE against one shared box, so what
    // depends only on the box and the view - the half-diagonal, the box centre, the lens terms - is computed
    // once, and the per-instance part (the three axis lengths, the centre through the transform, the distance,
    // the squared screen radius) runs with instances as lanes.
    //
    // BIT FOR BIT THE PER-INSTANCE glm FORMULA it replaced (IEEE single precision, no contraction under
    // /fp:precise): each lane does the same operations on the same values in the same order -
    //   scale    = glm::max( |c0|, glm::max( |c1|, |c2| ) ), |c| = sqrt( ( x*x + y*y ) + z*z ) (glm::length's
    //              dot order; sqrtps is the correctly rounded root), glm::max( x, y ) == Simd::Max( y, x );
    //   radius   = ( length( Max - Min ) * 0.5f ) * scale - the left-to-right product, its first factor shared;
    //   centre   = glm's mat4 * vec4, ( c0 * x + c1 * y ) + ( c2 * z + c3 * 1 ), with c3 * 1 == c3 exactly;
    //   distSq   = dot( centre - origin ) * |P[2][3]|;
    //   radiusSq = ( m * radius ) * ( m * radius ) / std::max( 1, distSq ), std::max( 1, d ) == Simd::Max( d, 1 )
    //              (a NaN distance takes 1 on both);
    // and the level walk is the scalar compare on the stored lane. A lane past the run holds zeros, never read.
    inline void SelectLODsFromBounds( const std::span<const glm::mat4> transforms,
                                      const Common::Math::AABB& localBounds, const LODView& view,
                                      const std::span<const float> screenSizes, const int forcedLOD,
                                      const int lodBias, const std::span<uint32_t> levels ) noexcept
    {
        const std::size_t count = std::min( transforms.size(), levels.size() );
        if ( forcedLOD >= 0 || screenSizes.size() <= 1 || IsEmpty( localBounds ) )
        {
            // Forced: that level; one level: nothing to choose; an empty mesh: LOD 0.
            uint32_t fixed = 0u;
            if ( forcedLOD >= 0 )
                fixed = static_cast<uint32_t>( forcedLOD );
            for ( std::size_t i = 0; i < count; ++i )
                levels[i] = fixed;
            return;
        }

        // World-space bounding sphere: the box's centre through the transform, radius = half-diagonal x the
        // largest transform scale. Size-aware: a large object keeps full detail farther away than a small one.
        namespace S                  = Common::Math::Simd;
        const float     halfDiagonal = glm::length( localBounds.Max - localBounds.Min ) * 0.5f;
        const glm::vec3 localCentre  = 0.5f * ( localBounds.Min + localBounds.Max );
        // BoundsScreenRadiusSquared's lens terms.
        const float distanceScale = std::abs( view.Projection[2][3] );
        const float multiple =
             std::max( 0.5f * std::abs( view.Projection[0][0] ), 0.5f * std::abs( view.Projection[1][1] ) );
        const int coarsest = static_cast<int>( screenSizes.size() ) - 1;

        constexpr std::size_t kLanes = 8;
        for ( std::size_t first = 0; first < count; first += kLanes )
        {
            const std::size_t lanes = std::min( kLanes, count - first );
            // The twelve terms of the transform the policy reads, lane per instance: columns 0..3, rows x, y, z.
            float terms[12][kLanes] = {};
            for ( std::size_t lane = 0; lane < lanes; ++lane )
            {
                const glm::mat4& t = transforms[first + lane];
                for ( int column = 0; column < 4; ++column )
                    for ( int row = 0; row < 3; ++row )
                        terms[column * 3 + row][lane] = t[column][row];
            }
            S::Float8 c[12];
            for ( int term = 0; term < 12; ++term )
                c[term] = S::Load8( terms[term] );
            const auto axisLength = [&c]( const int column )
            {
                const S::Float8 x = c[column * 3];
                const S::Float8 y = c[column * 3 + 1];
                const S::Float8 z = c[column * 3 + 2];
                return S::Sqrt( S::Add( S::Add( S::Mul( x, x ), S::Mul( y, y ) ), S::Mul( z, z ) ) );
            };
            const S::Float8 scale  = S::Max( S::Max( axisLength( 2 ), axisLength( 1 ) ), axisLength( 0 ) );
            const S::Float8 radius = S::Mul( S::Splat8( halfDiagonal ), scale );
            // Row r of mat4 * vec4( localCentre, 1 ), minus the eye's coordinate r.
            const auto offset = [&c, &localCentre, &view]( const int row )
            {
                const S::Float8 xy = S::Add( S::Mul( c[row], S::Splat8( localCentre.x ) ),
                                             S::Mul( c[3 + row], S::Splat8( localCentre.y ) ) );
                const S::Float8 zw = S::Add( S::Mul( c[6 + row], S::Splat8( localCentre.z ) ), c[9 + row] );
                return S::Sub( S::Add( xy, zw ), S::Splat8( view.Origin[row] ) );
            };
            const S::Float8 dx     = offset( 0 );
            const S::Float8 dy     = offset( 1 );
            const S::Float8 dz     = offset( 2 );
            const S::Float8 dot    = S::Add( S::Add( S::Mul( dx, dx ), S::Mul( dy, dy ) ), S::Mul( dz, dz ) );
            const S::Float8 distSq = S::Mul( dot, S::Splat8( distanceScale ) );
            const S::Float8 reach  = S::Mul( S::Splat8( multiple ), radius );
            const S::Float8 screenRadiusSq =
                 S::Div( S::Mul( reach, reach ), S::Max( distSq, S::Splat8( 1.0f ) ) );
            float radiusSq[kLanes];
            S::Store8( radiusSq, screenRadiusSq );
            for ( std::size_t lane = 0; lane < lanes; ++lane )
            {
                // UE ComputeStaticMeshLOD: the coarsest level whose screen size the object is still below.
                int base = 0;
                for ( int level = coarsest; level > 0; --level )
                {
                    const float half = 0.5f * screenSizes[static_cast<std::size_t>( level )];
                    if ( half * half > radiusSq[lane] )
                    {
                        base = level;
                        break;
                    }
                }
                // Per-mesh bias shifts the auto pick (+coarser / -finer).
                levels[first + lane] = static_cast<uint32_t>( std::clamp( base + lodBias, 0, coarsest ) );
            }
        }
    }

    // THE POLICY, taking bounds that were already computed: one instance through the batch's arithmetic (one
    // definition, so a single mesh and an ISM instance at the same transform cannot pick different levels).
    inline uint32_t SelectLODFromBounds( const glm::mat4& transform, const Common::Math::AABB& localBounds,
                                         const LODView& view, const std::span<const float> screenSizes,
                                         int forcedLOD, int lodBias )
    {
        uint32_t level = 0u;
        SelectLODsFromBounds( std::span<const glm::mat4>( &transform, 1u ), localBounds, view, screenSizes,
                              forcedLOD, lodBias, std::span<uint32_t>( &level, 1u ) );
        return level;
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
