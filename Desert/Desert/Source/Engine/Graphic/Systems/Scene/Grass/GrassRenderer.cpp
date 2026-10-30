// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Private/LandscapeGrass.cpp:2032-2080 (FGrassBuilderBase: the
// grid size from GrassDensity) and :2378-2520 (FAsyncGrassBuilder::Build, its grid branch), adapted: the instances
// are generated on the GPU every frame on a world-aligned grid around the view
// (Programs/Grass/GrassGenerate.shader) instead of being built on worker threads per landscape component into HISM
// tiles, and they are drawn by indirect draws through the mesh renderer's instanced cells (GpuInstances.hpp)
// instead of a HISM component.
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/TextureStreamingDemand.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <tuple>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGrassGroupSize       = 8; // GrassGenerate's LocalSize
        constexpr uint32_t kFlagRandomRotation   = 1u;
        constexpr uint32_t kFlagAlignToSurface   = 2u;
        constexpr uint32_t kFlagWeightAttenuates = 4u;
        constexpr uint32_t kFlagBlade            = 8u;
        // GpuInstanceBucket: count, first, capacity, and a mesh level's half screen size (bit pattern; 0 for a blade).
        constexpr uint32_t kBucketWords = 4u;
        // GRASS-VIS: CameraArgs' words per bucket after its bucket table (GrassGenerate.shader CameraArgs):
        // candidates appended, candidates frozen, phase 1's drawn, phase 2's appended.
        constexpr uint32_t kPhaseWords = 4u;
        // GRASS-VIS-7: Feedback's words per variety past its bucket counts (GrassFinalize.shader Feedback): the
        // written mark, the full list's drawn count, the camera's phase-1 drawn count, phase 2's appended count.
        constexpr uint32_t kFeedbackTailWords = 4u;
        // A camera candidate's two uvec4 (GrassGenerate.shader Candidates): its sphere and its row of Instances.
        constexpr uint32_t kCandidateBytes = 32u;
        // GrassGenerate's phase 2 runs one thread per slot (64 a group); its groups wrap at this many along x.
        constexpr uint32_t kPostGroupsX = 1024u;

        // Mirrors GrassGenerate's GrassParams (std430).
        struct GrassParamsGpu
        {
            glm::vec4  Planes[4];       // the full list's volume (the camera's side planes swept along the lights)
            glm::vec4  CameraPlanes[4]; // the camera's side planes (its own list)
            glm::vec4  Camera;
            glm::vec4  Cull;
            glm::vec4  Range;
            glm::vec4  ScaleX;
            glm::vec4  ScaleY;
            glm::vec4  ScaleZ;
            glm::uvec4 Info;
            glm::uvec4 Buckets; // x = bucket count, y = the bucket table's first word in Args
            glm::vec4  Blade;   // x = LodDistance (0: one bucket), y = ClumpSize, z = Clumping, w = ClumpPull
            glm::vec4  Spread;  // x = Scatter (cells), y = Patchiness, z = PatchSize
            glm::vec4  Lean;    // x = Tilt (radians), y = FacingJitter (radians), z = the world's SnowDepth (cm)
            // A mesh's per-instance level by screen size (Geometry::BoundsScreenRadiusSquared): x = the lens's
            // screen multiple x the mesh's local half-diagonal, y = |Projection[2][3]| (0: orthographic).
            glm::vec4 Lod;
            // The variety's world patches: x = Coverage (1: none), y = Size cm, z = Softness, w = the noise seed's
            // bits (shared by a Group).
            glm::vec4 Patches;
        };
        static_assert( sizeof( GrassParamsGpu ) == 336 );

        // Instances whose cell centre lies within [inner, outer) of the camera, the ring widened on each side by
        // one jittered cell and by @p moved, the farthest a blade's scatter and clump pull carry it from its cell.
        uint32_t RingCapacity( double inner, double outer, double cell, double moved )
        {
            const double lo = std::max( 0.0, inner - 1.5 * cell - moved );
            const double hi = outer + 1.5 * cell + moved;
            return static_cast<uint32_t>( std::ceil( 3.14159265 * ( hi * hi - lo * lo ) / ( cell * cell ) ) );
        }

        // THE LARGEST GROUND AREA THE PART OF A VIEW'S PYRAMID WITHIN @p reach OF THE EYE CAN COVER, widened by
        // @p margin on every side, for any orientation of the view. A capacity sized for the whole disc of the
        // cull radius grows with the square of the lens (DrawDistanceScale stretches the radius k times), while
        // the pyramid a lens of half-angle tangents (@p tanX, @p tanY) sees grows only linearly with k: a
        // 12-degree scope keeps ~9.5x the distance but sees a ~1/9.5 wedge of it. Bounds, not estimates - the
        // capacity must hold every instance the generator can keep, or the farthest are dropped by GrassFinalize's
        // clamp:
        //  - an orthogonal projection of a convex body covers at most half its surface area (Cauchy), and the part
        //    of the pyramid within reach of its apex lies inside the pyramid cut at depth `reach`, whose surface
        //    is the base 4 r^2 tx ty plus four triangles r^2 tx sqrt(1 + ty^2) and r^2 ty sqrt(1 + tx^2), two
        //    each;
        //  - that projection is convex and lies inside the disc of radius `reach`, so its perimeter is at most
        //    2 pi reach, and the margin adds perimeter x margin + pi margin^2 (Steiner's formula).
        double ViewFootprintArea( double reach, double tanX, double tanY, double margin )
        {
            constexpr double kPi     = 3.14159265358979;
            const double     surface = reach * reach *
                                   ( 4.0 * tanX * tanY + 2.0 * tanX * std::sqrt( 1.0 + tanY * tanY ) +
                                     2.0 * tanY * std::sqrt( 1.0 + tanX * tanX ) );
            const double inside = std::min( 0.5 * surface, kPi * reach * reach );
            return inside + 2.0 * kPi * reach * margin + kPi * margin * margin;
        }

        // A bucket's capacity rounded up to the next power of sqrt(2). The lens changes every frame while a scope
        // zooms, and an exact capacity would reallocate the variety's buffers on every one of those frames; on the
        // steps it reallocates about seven times across a 90-to-12-degree zoom and never wastes more than 41 %.
        uint32_t CapacityStep( uint32_t needed )
        {
            if ( needed == 0u )
                return 0u;
            const double step = std::exp2( 0.5 * std::ceil( 2.0 * std::log2( static_cast<double>( needed ) ) ) );
            return std::max( needed, static_cast<uint32_t>( std::ceil( step ) ) );
        }

        // UE FGrassBuilderBase: SqrtMaxInstances = sqrt(Extent^2 * GrassDensity / 1000 / 1000), i.e. one instance
        // per (1e6 / Density) cm^2 - the grid cell's side.
        float CellSizeCm( const GrassPlacement& placement )
        {
            return std::sqrt( 1.0e6f / placement.Density );
        }

        // The view's four side planes (Gribb-Hartmann on the unjittered ViewProjection), normalised; the near and
        // far planes are left to the cull distance, so the depth convention does not matter.
        void SidePlanes( const glm::mat4& viewProjection, glm::vec4 ( &planes )[4] )
        {
            const glm::vec4 row0( viewProjection[0][0], viewProjection[1][0], viewProjection[2][0],
                                  viewProjection[3][0] );
            const glm::vec4 row1( viewProjection[0][1], viewProjection[1][1], viewProjection[2][1],
                                  viewProjection[3][1] );
            const glm::vec4 row3( viewProjection[0][3], viewProjection[1][3], viewProjection[2][3],
                                  viewProjection[3][3] );
            const glm::vec4 raw[4] = { row3 + row0, row3 - row0, row3 + row1, row3 - row1 };
            for ( int i = 0; i < 4; ++i )
                planes[i] = raw[i] / glm::length( glm::vec3( raw[i] ) );
        }

        // GRASS-VIS-6: @p planes (normalised, inside where n.p + w >= 0) swept along every light of @p sweeps by its
        // reach: an instance at p darkens p + t * Travel for t in [0, Reach], which stays inside plane i for some t
        // exactly when n.p + w + Reach * max(0, n.Travel) >= 0 - so each plane moves out by its largest such term.
        // Per plane, so the volume holds the swept camera volume of every light (a little more where two planes'
        // sweeps meet). True when a plane moved.
        template <size_t N>
        bool SweepPlanes( glm::vec4 ( &planes )[4],
                          const std::array<MeshRenderer::DirectionalCasterSweep, N>& sweeps )
        {
            bool moved = false;
            for ( glm::vec4& plane : planes )
            {
                float out = 0.0f;
                for ( const MeshRenderer::DirectionalCasterSweep& sweep : sweeps )
                    out = std::max( out,
                                    sweep.Reach * std::max( 0.0f, glm::dot( glm::vec3( plane ), sweep.Travel ) ) );
                plane.w += out;
                moved = moved || out > 0.0f;
            }
            return moved;
        }
        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct GrassGenerateShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Field );
            DESERT_SHADER_PARAMETER( StorageBuffer, Params );
            DESERT_SHADER_PARAMETER( StorageBuffer, Instances );
            DESERT_SHADER_PARAMETER( StorageBuffer, Args );
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( Texture, GrassHzbTexture );
            DESERT_SHADER_PARAMETER( StorageBuffer, CameraInstances );
            DESERT_SHADER_PARAMETER( StorageBuffer, CameraArgs );
            DESERT_SHADER_PARAMETER( StorageBuffer, Candidates );
        };

        // GrassGenerate's push block, std430 (Programs/Grass/GrassGenerate.shader PushConstants).
        struct GrassGeneratePush
        {
            glm::ivec4 CellRange{ 0 }; // phase 2: x = candidate slots, y = groups along x
            glm::mat4  HzbViewProjection{ 1.0f };
            glm::uvec4 Hzb{ 0u }; // xy = mip-0 size, z = mip count, w = the phase (0 untested, 1, 2)
        };
        static_assert( sizeof( GrassGeneratePush ) == 96 );

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct GrassFinalizeShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Args );
            DESERT_SHADER_PARAMETER( StorageBuffer, Feedback );
            DESERT_SHADER_PARAMETER( StorageBuffer, CameraArgs );
            DESERT_SHADER_PARAMETER( StorageBuffer, PostArgs );
        };

        // GrassFinalize's push block (Programs/Grass/GrassFinalize.shader PushConstants).
        struct GrassFinalizePush
        {
            glm::uvec4 Mode{ 0u }; // x = 0 after the generation, 1 after phase 2; y = 1 with a camera list
        };
        static_assert( sizeof( GrassFinalizePush ) == 16 );

        // WHAT A VARIETY ASKS OF ITS TEXTURES (TEX-STREAMING). UE streams a grass HISM's textures like any
        // primitive's: the distance from the view to the component's bounds, the mesh's size at that distance
        // (FRenderAssetInstanceView). The instances live only on the GPU here, so the nearest one is taken where
        // it can grow: the field's ground nearest the camera (its sample under the eye, the eye clamped onto the
        // field), out to the variety's EndCullDistance through this lens. Without it every grass texture kept the
        // tail it was loaded with (mip 4 of a 2K atlas) at the camera's feet: a near patch of thin grey stalks.
        void NoteVarietyTextureDemand( const GrassVarietyDraw& variety, const Desert::Mesh& mesh,
                                       const GrassFieldData& field, const ViewFrame& frame )
        {
            const std::vector<uint32_t>& w          = field.Words;
            const uint32_t               viewHeight = frame.Split.Output.Height;
            if ( w.size() < 8 || w[0] == 0 || w[1] == 0 || viewHeight == 0 )
                return;
            const glm::vec3 eye     = frame.CameraPosition;
            const glm::vec2 origin  = { std::bit_cast<float>( w[4] ), std::bit_cast<float>( w[5] ) };
            const float     spacing = std::bit_cast<float>( w[6] );
            if ( !( spacing > 0.0f ) || w.size() < 8ull + static_cast<uint64_t>( w[0] ) * w[1] )
                return;
            const auto sampleOf = [&]( const float at, const float from, const uint32_t count )
            {
                const float i = std::round( ( at - from ) / spacing );
                return static_cast<uint32_t>( std::clamp( i, 0.0f, static_cast<float>( count - 1 ) ) );
            };
            const uint32_t sx     = sampleOf( eye.x, origin.x, w[0] );
            const uint32_t sz     = sampleOf( eye.z, origin.y, w[1] );
            const float    height = std::bit_cast<float>( w[8 + static_cast<size_t>( sz ) * w[0] + sx] );
            // No tile loaded under the nearest sample: the ground is not known, so the eye's own height stands for
            // it (the nearest an instance can be is then the camera itself - all the mips, never too few).
            const glm::vec3         ground = { origin.x + static_cast<float>( sx ) * spacing,
                                       height == kGrassNoHeight ? eye.y : height,
                                               origin.y + static_cast<float>( sz ) * spacing };
            const Geometry::LODView view{ eye, frame.Projection };
            if ( glm::length( ground - eye ) > variety.Placement.EndCullDistance * DrawDistanceScale( view ) )
                return;
            const GrassPlacement& p        = variety.Placement;
            const float           maxScale = std::max( { p.ScaleX.y, p.ScaleY.y, p.ScaleZ.y, 1.0e-3f } );
            float                 radius   = 0.0f;
            if ( variety.Blade )
                radius = 0.5f * variety.Blade->Height;
            else
            {
                const Common::Math::AABB bounds = Geometry::LocalBounds( mesh.GetSubmeshes() );
                radius                          = 0.5f * glm::length( bounds.Max - bounds.Min );
            }
            const float pixels = TextureStreamingDemand::ScreenPixels( ground + glm::vec3( 0.0f, radius, 0.0f ),
                                                                       radius * maxScale, view, viewHeight );
            for ( const GrassSectionDraw& section : variety.Sections )
                if ( section.Material )
                    TextureStreamingDemand::Get().Note( section.Material->GetParentMaterial(), pixels );
        }

    } // namespace

    Common::BoolResultStr GrassRenderer::Initialize()
    {
        auto* shaders = Runtime::ResourceRegistry::GetShaderService();
        for ( const auto& [name, into] :
              { std::pair{ "GrassGenerate", &m_Generate }, std::pair{ "GrassFinalize", &m_Finalize } } )
        {
            const auto shader = shaders->GetByName( name );
            if ( !shader )
                return Common::MakeError( std::format( "GrassRenderer: missing shader '{}'", name ) );
            const auto made = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !made )
                return Common::MakeError( std::format( "GrassRenderer: {}", made.GetError() ) );
            *into = made.GetValue();
        }
        return BOOLSUCCESS;
    }

    Desert::Mesh* GrassRenderer::BladeStrip( const uint32_t segments )
    {
        auto& strip = m_BladeStrips[segments];
        if ( strip )
            return strip.get();
        // A strip in blade space: Position = (side -1/+1, height share 0..1, 0); the vertex stage bends, tapers
        // and places it (Vertex_GrassBlade.glslh). The normal frame is the stage's too.
        std::vector<Vertex>  vertices;
        std::vector<Index>   indices;
        std::vector<Submesh> submeshes;
        for ( uint32_t level = segments;; level = std::max( 1u, level / 2u ) )
        {
            Submesh sm{};
            sm.Name         = std::format( "Blade{}", level );
            sm.VertexOffset = static_cast<uint32_t>( vertices.size() );
            sm.IndexOffset  = static_cast<uint32_t>( indices.size() * 3u );
            const GrassBladeStripLevel blade = MakeGrassBladeStripLevel( level );
            sm.VertexCount                   = static_cast<uint32_t>( blade.Vertices.size() );
            sm.IndexCount                    = static_cast<uint32_t>( blade.Indices.size() * 3u );
            vertices.insert( vertices.end(), blade.Vertices.begin(), blade.Vertices.end() );
            indices.insert( indices.end(), blade.Indices.begin(), blade.Indices.end() );
            sm.Transform   = glm::mat4( 1.0f );
            sm.BoundingBox = Common::Math::AABB{ glm::vec3( -1.0f, 0.0f, 0.0f ), glm::vec3( 1.0f, 1.0f, 0.0f ) };
            submeshes.push_back( std::move( sm ) );
            if ( level == 1u )
                break;
        }
        strip = std::make_shared<StaticMesh>( vertices, indices, submeshes, std::vector<MeshVertexStreams>{} );
        if ( const auto uploaded = strip->Invalidate(); !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[Grass] the {}-segment blade strip was not uploaded: {}", segments, uploaded.GetError() );
            strip.reset();
            return nullptr;
        }
        return strip.get();
    }

    GrassRenderer::VarietyGpu* GrassRenderer::PrepareVariety( const GrassVarietyDraw& variety, Desert::Mesh& mesh,
                                                              const ViewFrame& frame, const ViewVolumes& volumes )
    {
        const GrassPlacement& p    = variety.Placement;
        const float           cell = CellSizeCm( p );
        // Every distance of the field is authored for a 90-degree lens and read through this view's (UE scales the
        // grass HISM's cull distances by CalculateFieldOfViewDistanceScale the same way): a zoomed scope draws the
        // field out to k times its EndCullDistance and picks the blade's levels at k times its LodDistance, so a
        // patch at 100 m through a 12-degree lens looks as it does at ~10 m through a 90-degree one.
        const Geometry::LODView lensView{ frame.CameraPosition, frame.Projection };
        const float             lens        = DrawDistanceScale( lensView );
        const bool              perspective = std::abs( frame.Projection[2][3] ) != 0.0f;
        const double            tanX        = 1.0 / std::max( 1.0e-6f, std::abs( frame.Projection[0][0] ) );
        const double            tanY        = 1.0 / std::max( 1.0e-6f, std::abs( frame.Projection[1][1] ) );
        // The lens's wedge bounds what is generated (the cells, the capacity) only while the full list is the
        // camera's own volume: swept along a light (GRASS-VIS-6) it reaches past the wedge, anywhere in the disc.
        const bool              wedge       = perspective && !volumes.Swept;
        const float             radius      = p.EndCullDistance * lens;
        // The floor of the share of cells generated (GrassGenerate LensShare): 1 / k through a zoomed lens - flat
        // ground far away, where it holds the authored lens's instances per pixel; slopes facing the camera and
        // ground under it keep more, up to every cell.
        const float             keepShare   = std::min( 1.0f, 1.0f / std::max( lens, 1.0e-6f ) );
        const auto&             submeshes   = mesh.GetSubmeshes();
        // A blade draws one level per bucket (submesh b = bucket b, by distance). A mesh draws one bucket per
        // level of its LOD chain, every instance in the coarsest level whose screen size it is still below (UE
        // grass HISM: the instance's LOD by ComputeStaticMeshLOD's screen size, Geometry::SelectLODFromBounds) -
        // the levels are the mesh asset's own, its records (level L x submesh) each draw that level's index range.
        const std::vector<float>& screenSizes = mesh.GetLODScreenSizes();
        const uint32_t            levels =
             variety.Blade ? 1u
                                      : std::max( 1u, std::min( Geometry::MaxAvailableLOD( submeshes ) + 1u,
                                                                static_cast<uint32_t>( screenSizes.size() ) ) );
        const uint32_t buckets     = variety.Blade ? static_cast<uint32_t>( submeshes.size() ) : levels;
        const auto     records     = static_cast<uint32_t>( submeshes.size() ) * levels;
        const float    lodDistance = variety.Blade ? variety.Blade->LodDistance * lens : 0.0f;
        // The instance's bounding sphere in its local space: a blade's from its shape (the upright blade's
        // middle, reaching the farthest leaning tip - its own curvature draw and its root's tilt - and the wind's
        // sway), a mesh's from its bounds.
        float boundsRadius = 0.0f, boundsHeight = 0.0f;
        if ( variety.Blade )
        {
            const GrassBladeDraw& b = *variety.Blade;
            boundsHeight            = 0.5f * b.Height;
            boundsRadius            = boundsHeight * ( 1.0f + b.Curvature * ( 1.0f + b.CurvatureSpread ) ) +
                           b.Height * std::sin( glm::radians( b.Tilt ) ) + b.MaxWidth + variety.Wind.Strength;
        }
        else
        {
            const Common::Math::AABB bounds = Geometry::LocalBounds( submeshes );
            const glm::vec3          centre = ( bounds.Min + bounds.Max ) * 0.5f;
            boundsHeight                    = centre.y;
            boundsRadius =
                 glm::length( bounds.Max - bounds.Min ) * 0.5f + glm::length( glm::vec2( centre.x, centre.z ) );
        }
        // The nearest clump point lies within the 3x3 clump cells around the blade: at most 2 * sqrt(2) cells off;
        // scatter carries the blade up to 2 * Scatter grid cells before that.
        const double moved = variety.Blade
                                  ? 2.0 * 1.41421356 * variety.Blade->ClumpSize * variety.Blade->ClumpPull +
                                         2.0 * variety.Blade->Scatter * cell
                                  : 0.0;
        // How far a kept instance's cell centre can lie outside the view's pyramid: the side-plane test passes a
        // bounding sphere that only touches it, and the cell centre is up to a jittered cell and `moved` off the
        // instance (one more cell for counting lattice points rather than area).
        const double margin = static_cast<double>( boundsRadius ) + 2.5 * cell + moved;
        VarietyGpu&  gpu    = m_Varieties[variety.Key];
        // What the GPU counted for each bucket FramesInFlight frames ago (GrassFinalize), when this frame's copy
        // has been written since the feedback buffer was made for this bucket count.
        std::vector<uint32_t> counted( buckets + kFeedbackTailWords, 0u );
        bool                  fedBack = false;
        gpu.Drawn                     = std::nullopt;
        if ( gpu.Feedback && gpu.Buckets == buckets )
        {
            if ( const auto read = gpu.Feedback->ReadBack( counted.data(), ( buckets + kFeedbackTailWords ) * 4u );
                 !read.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the generated counts were not read back: {}", read.GetError() );
            }
            else
            {
                fedBack = counted[buckets] != 0u;
                if ( fedBack )
                    gpu.Drawn = glm::uvec3( counted[buckets + 1u], counted[buckets + 2u], counted[buckets + 3u] );
            }
        }
        if ( gpu.LowFrames.size() != buckets )
            gpu.LowFrames.assign( buckets, 0u );

        // Per bucket the capacity holds what the generator ACTUALLY keeps, not what it could (UE grass: a
        // component's instance buffer is sized by the instances its builder produced, never by density x area).
        // Every cell whose jittered instance can lie within the bucket's ring of the cull radius - and, through
        // a perspective lens, within what that lens can see of the ring - only BOUNDS it (`bound`, every cell
        // kept): the layer's weight, the density range, slopes and the lens share drop most of those cells, and
        // sizing for the bound held 932 MB of instances on FIELD_Barn (FIELD-MEM) for a fraction drawn. The
        // GPU's own uncapped count of last frames (GrassFinalize) says what was kept: the capacity follows it,
        // growing at once to the count plus a quarter, shrinking only after kShrinkFrames frames in a row two
        // steps (half) above it. Until the first count comes back (FramesInFlight frames after the variety
        // appears) the capacity is zero: those frames only count, and the field appears once it is measured.
        constexpr uint32_t    kShrinkFrames = 60;
        std::vector<uint32_t> capacities( buckets );
        uint32_t              capacity = 0;
        // The first bucket whose read-back count exceeds the capacity it was drawn with (GrassGenerate dropped the
        // rest): the frames since drew a truncated field (InstancesSettling).
        std::optional<uint32_t> overflowed;
        for ( uint32_t b = 0; b < buckets; ++b )
        {
            // A blade's level is a ring of distance; a mesh level's screen size reaches anywhere in the disc (a
            // large instance keeps its detail farther than a small one), so it is bounded by the whole disc.
            const bool   ring  = variety.Blade && buckets > 1u;
            const double inner = ring ? static_cast<double>( b ) * lodDistance : 0.0;
            const double outer = !ring || b + 1u == buckets ? static_cast<double>( radius )
                                                            : std::min<double>( radius, ( b + 1.0 ) * lodDistance );
            uint32_t     needed = inner >= radius ? 0u : RingCapacity( inner, outer, cell, moved );
            if ( wedge && needed > 0u )
                needed = std::min<uint32_t>(
                     needed, static_cast<uint32_t>( std::ceil( ViewFootprintArea( outer, tanX, tanY, margin ) /
                                                               ( static_cast<double>( cell ) * cell ) ) ) );
            const uint32_t bound  = needed;
            const uint32_t target = fedBack ? std::min( bound, counted[b] + counted[b] / 4u + 64u ) : 0u;
            const uint32_t step = CapacityStep( target );
            const uint32_t held = gpu.BucketCapacities.size() == buckets ? gpu.BucketCapacities[b] : 0u;
            if ( fedBack && counted[b] > held && !overflowed )
                overflowed = b;
            if ( step >= held || held > CapacityStep( bound ) )
            {
                capacities[b]    = step;
                gpu.LowFrames[b] = 0u;
            }
            else if ( 2u * step <= held && ++gpu.LowFrames[b] >= kShrinkFrames )
            {
                capacities[b]    = step;
                gpu.LowFrames[b] = 0u;
            }
            else
            {
                capacities[b] = held;
                if ( 2u * step > held )
                    gpu.LowFrames[b] = 0u;
            }
            capacity += capacities[b];
        }

        // Settling (InstancesSettling): nothing counted yet (capacity zero, the field is not drawn), or an
        // overflow read now - the frames still in flight were drawn with the old capacity too, so the variety
        // settles only after that many read-backs within capacity.
        const uint32_t settleReads = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( !fedBack )
            gpu.SettleReads = settleReads;
        else if ( overflowed )
        {
            if ( !gpu.Truncated )
                LOG_INFO( "[Grass] variety {:016x} (layer {}) truncated {}->{} in bucket {} of {}: the field drew "
                          "without the rest until the capacity grows",
                          variety.Key, variety.Layer, counted[*overflowed],
                          gpu.BucketCapacities.size() == buckets ? gpu.BucketCapacities[*overflowed] : 0u,
                          *overflowed, buckets );
            gpu.Truncated   = true;
            gpu.SettleReads = settleReads;
        }
        else if ( gpu.SettleReads > 0u && --gpu.SettleReads == 0u )
            gpu.Truncated = false;
        m_Settling = m_Settling || gpu.SettleReads > 0u;

        if ( gpu.Mesh != &mesh || gpu.Capacity != capacity || gpu.Buckets != buckets ||
             gpu.BucketCapacities != capacities || !gpu.Instances || gpu.List != volumes.List )
        {
            gpu.List             = volumes.List;
            gpu.Mesh             = &mesh;
            gpu.Capacity         = capacity;
            gpu.Buckets          = buckets;
            gpu.BucketCapacities = capacities;
            if ( gpu.Index == 0 )
                gpu.Index = ++m_NextIndex;
            // One instance's room even at zero capacity: a counting-only frame still binds the buffer.
            gpu.Instances           = ShaderResources::StorageBuffer::Create(
                 "GrassInstances", std::max( capacity, 1u ) * sizeof( glm::mat4 ), 2, true );
            const auto submeshCount = static_cast<uint32_t>( submeshes.size() );
            const auto tableWord    = static_cast<uint32_t>( GpuInstanceArgsOffset( records ) / 4u );
            const auto argBytes     = ( tableWord + buckets * kBucketWords ) * 4u;
            gpu.Args                = ShaderResources::StorageBuffer::Create( "GrassArgs", argBytes, 3, true );
            // The records' static half: record L x submeshCount + si draws submesh si at level L (a submesh with a
            // shorter chain its coarsest level, one without a chain its base range; a blade has level 0 only);
            // instanceCount and firstInstance are GrassFinalize's. The bucket table: each bucket's first instance,
            // capacity and - a mesh level - half its screen size.
            std::vector<uint32_t> args( argBytes / 4u, 0u );
            args[0] = buckets;
            args[1] = capacity;
            args[2] = records;
            for ( uint32_t level = 0; level < levels; ++level )
                for ( uint32_t si = 0; si < submeshCount; ++si )
                {
                    const auto& sm     = submeshes[si];
                    uint32_t*   record = &args[GpuInstanceArgsOffset( level * submeshCount + si ) / 4u];
                    if ( sm.LODs.empty() )
                    {
                        record[0] = sm.IndexCount;
                        record[2] = sm.IndexOffset;
                    }
                    else
                    {
                        const auto& lod = sm.LODs[std::min<size_t>( level, sm.LODs.size() - 1 )];
                        record[0]       = lod.IndexCount;
                        record[2]       = lod.IndexOffset;
                    }
                    record[3] = sm.VertexOffset;
                }
            for ( uint32_t b = 0, first = 0; b < buckets; first += capacities[b], ++b )
            {
                args[tableWord + b * kBucketWords + 1u] = first;
                args[tableWord + b * kBucketWords + 2u] = capacities[b];
                if ( !variety.Blade )
                    args[tableWord + b * kBucketWords + 3u] = std::bit_cast<uint32_t>( 0.5f * screenSizes[b] );
            }
            if ( const auto wrote = gpu.Args->SetData( args.data(), argBytes ); !wrote.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the argument records were not written: {}", wrote.GetError() );
                return nullptr;
            }
            // The camera's list (GRASS-VIS): the same records and bucket table, its own counts; CameraArgs adds
            // the phase words, and only an HZB-tested list has candidates and phase 2's records (PlanesAndHzb).
            // What this frame's list does not use is a one-word stand-in (the passes bind it, Hzb.w / Mode.y keep
            // it idle).
            if ( volumes.List != CameraList::None )
            {
                gpu.CameraInstances = ShaderResources::StorageBuffer::Create(
                     "GrassCameraInstances", std::max( capacity, 1u ) * sizeof( glm::mat4 ), 5, true );
                std::vector<uint32_t> cameraArgs = args;
                cameraArgs.resize( args.size() + static_cast<size_t>( buckets ) * kPhaseWords, 0u );
                const auto cameraBytes = static_cast<uint32_t>( cameraArgs.size() * 4u );
                gpu.CameraArgs = ShaderResources::StorageBuffer::Create( "GrassCameraArgs", cameraBytes, 6, true );
                if ( const auto wrote = gpu.CameraArgs->SetData( cameraArgs.data(), cameraBytes ); !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the camera list's records were not written: {}", wrote.GetError() );
                    return nullptr;
                }
            }
            else
            {
                gpu.CameraInstances = ShaderResources::StorageBuffer::Create( "GrassCameraInstances", 16u, 5, true );
                gpu.CameraArgs      = ShaderResources::StorageBuffer::Create( "GrassCameraArgs", 16u, 6, true );
            }
            if ( volumes.List == CameraList::PlanesAndHzb )
            {
                gpu.Candidates = ShaderResources::StorageBuffer::Create(
                     "GrassCandidates", std::max( capacity, 1u ) * kCandidateBytes, 7, true );
                gpu.PostArgs = ShaderResources::StorageBuffer::Create( "GrassPostArgs", argBytes, 3, true );
                if ( const auto wrote = gpu.PostArgs->SetData( args.data(), argBytes ); !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the camera list's phase-2 records were not written: {}", wrote.GetError() );
                    return nullptr;
                }
            }
            else
            {
                gpu.Candidates = ShaderResources::StorageBuffer::Create( "GrassCandidates", 16u, 7, true );
                gpu.PostArgs   = ShaderResources::StorageBuffer::Create( "GrassPostArgs", 16u, 3, true );
            }
            gpu.Params = ShaderResources::StorageBuffer::Create( "GrassParams", sizeof( GrassParamsGpu ), 1 );
            if ( const uint32_t feedbackBytes = ( buckets + kFeedbackTailWords ) * 4u;
                 !gpu.Feedback || gpu.Feedback->GetSize() != feedbackBytes )
                gpu.Feedback = ShaderResources::StorageBuffer::Create( "GrassFeedback", feedbackBytes, 1 );
            gpu.Shape.reset();
            if ( variety.Blade )
                gpu.Shape =
                     ShaderResources::StorageBuffer::Create( "GrassBladeShape", sizeof( GrassBladeShapeGpu ), 13 );
        }
        if ( variety.Blade )
        {
            const GrassBladeDraw&    b = *variety.Blade;
            const size_t             cards = std::min( b.Cards.size(), gpu.ShapeData.Cards.size() );
            gpu.ShapeData =
                 GrassBladeShapeGpu{ glm::vec4( b.Height, b.Stiffness, b.Curvature, static_cast<float>( cards ) ),
                                     glm::vec4( b.CurvatureSpread, b.BendSpread, 0.0f, 0.0f ) };
            std::copy_n( b.Cards.begin(), cards, gpu.ShapeData.Cards.begin() );
            // The variety's rows of this frame's raster stream (GRASS-VIS): instance i is row VisibilityRow + i
            // of the view's visibility buffer, the bucket table tells the resolve which strip level drew it.
            // Past kGrassVisibilityMaxRows the view refuses the path this frame (VisibilityRowsOverflow), so a
            // row cut to 32 bits here is never resolved.
            gpu.VisibilityRow = static_cast<uint32_t>( m_VisibilityRows );
            m_VisibilityRows += std::max( capacity, 1u );
            gpu.ShapeData.Visibility = glm::uvec4( gpu.VisibilityRow, b.Segments,
                                                   std::min( buckets, kGrassBladeVisibilityMaxBuckets ), 0u );
            for ( uint32_t bucket = 0, first = 0; bucket < std::min( buckets, kGrassBladeVisibilityMaxBuckets );
                  first += capacities[bucket], ++bucket )
                gpu.ShapeData.BucketFirst[bucket / 4u][bucket % 4u] = first;
            if ( const auto wrote = gpu.Shape->SetData( &gpu.ShapeData, sizeof( gpu.ShapeData ) );
                 !wrote.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the blade shape was not written: {}", wrote.GetError() );
                return nullptr;
            }
        }

        GrassParamsGpu params{};
        std::copy( std::begin( volumes.Full ), std::end( volumes.Full ), std::begin( params.Planes ) );
        std::copy( std::begin( volumes.Camera ), std::end( volumes.Camera ), std::begin( params.CameraPlanes ) );
        params.Camera = glm::vec4( frame.CameraPosition, radius );
        params.Cull   = glm::vec4( p.StartCullDistance * lens, radius, cell,
                                   std::clamp( p.PlacementJitter, 0.0f, 0.99f ) * 0.5f );
        params.Range = glm::vec4( p.AllowedDensityRange.x, p.AllowedDensityRange.y, p.MaxScaleWeightAttenuation,
                                  boundsRadius );
        const uint32_t flags =
             ( p.RandomRotation ? kFlagRandomRotation : 0u ) | ( p.AlignToSurface ? kFlagAlignToSurface : 0u ) |
             ( p.WeightAttenuatesMaxScale ? kFlagWeightAttenuates : 0u ) | ( variety.Blade ? kFlagBlade : 0u );
        params.ScaleX  = glm::vec4( p.ScaleX, static_cast<float>( p.Scaling ), std::bit_cast<float>( flags ) );
        params.ScaleY  = glm::vec4( p.ScaleY, boundsHeight, 0.0f );
        params.ScaleZ  = glm::vec4( p.ScaleZ, 0.0f, keepShare );
        params.Info    = glm::uvec4( variety.Layer, capacity, static_cast<uint32_t>( submeshes.size() ),
                                     static_cast<uint32_t>( variety.Key * 0x9E3779B97F4A7C15ull >> 32 ) );
        params.Buckets = glm::uvec4( buckets, static_cast<uint32_t>( GpuInstanceArgsOffset( records ) / 4u ), 0u, 0u );
        params.Blade = variety.Blade ? glm::vec4( lodDistance, variety.Blade->ClumpSize, variety.Blade->Clumping,
                                                  variety.Blade->ClumpPull )
                                     : glm::vec4( 0.0f );
        params.Spread = variety.Blade ? glm::vec4( 2.0f * variety.Blade->Scatter, variety.Blade->Patchiness,
                                                   variety.Blade->PatchSize, 0.0f )
                                      : glm::vec4( 0.0f, 0.0f, 1.0f, 0.0f );
        params.Lean   = variety.Blade ? glm::vec4( glm::radians( variety.Blade->Tilt ),
                                                   glm::radians( variety.Blade->FacingJitter ), 0.0f, 0.0f )
                                      : glm::vec4( 0.0f );
        {
            const uint32_t seed =
                 p.PatchGroup != 0u ? ( p.PatchGroup * 0x9E3779B1u ) ^ 0x68E31DA4u : params.Info.w ^ 0x2C1B3C6Du;
            params.Patches = glm::vec4( std::clamp( p.PatchCoverage, 0.0f, 1.0f ), std::max( p.PatchSize, 1.0f ),
                                        std::clamp( p.PatchSoftness, 0.0f, 1.0f ), std::bit_cast<float>( seed ) );
        }
        // The world's snow buries every instance's root by its depth (Common/WeatherSnow.glslh): a blade shows only
        // what stands above the snow, and one shorter than it is gone, in every pass that draws or shades it.
        params.Lean.z = std::max( m_SceneRenderer->GetWorldParameterScalar( "SnowDepth" ), 0.0f );
        if ( !variety.Blade )
        {
            const Common::Math::AABB bounds   = Geometry::LocalBounds( submeshes );
            const float              multiple = std::max( 0.5f * std::abs( frame.Projection[0][0] ),
                                                          0.5f * std::abs( frame.Projection[1][1] ) );
            params.Lod = glm::vec4( multiple * 0.5f * glm::length( bounds.Max - bounds.Min ),
                                    std::abs( frame.Projection[2][3] ), 0.0f, 0.0f );
        }
        if ( const auto wrote = gpu.Params->SetData( &params, sizeof( params ) ); !wrote.IsSuccess() )
        {
            LOG_ERROR( "[Grass] the variety's parameters were not written: {}", wrote.GetError() );
            return nullptr;
        }

        // The cells generated: the box of the cull disc, cut to the box of what the lens sees of it. Through a
        // perspective lens the part of the pyramid within `radius` of the eye lies in the hull of the eye and the
        // pyramid's four corner rays at `radius`, bulged by at most radius x (1 - cos) of the corner ray's angle
        // (the sphere's cap between them). A 12-degree scope then dispatches its wedge, not the 9.5x-wider disc.
        const glm::vec2 eye = glm::vec2( frame.CameraPosition.x, frame.CameraPosition.z );
        const auto      pad = static_cast<float>( margin );
        glm::vec2       lo  = eye - ( radius + pad );
        glm::vec2       hi  = eye + ( radius + pad );
        if ( wedge )
        {
            const glm::mat3 toWorld( frame.InvView );
            glm::vec2       seenLo = eye, seenHi = eye;
            for ( const glm::vec2 corner :
                  { glm::vec2( 1, 1 ), glm::vec2( 1, -1 ), glm::vec2( -1, 1 ), glm::vec2( -1, -1 ) } )
            {
                const glm::vec3 ray =
                     toWorld * glm::normalize( glm::vec3( corner.x * static_cast<float>( tanX ),
                                                          corner.y * static_cast<float>( tanY ), -1.0f ) );
                const glm::vec2 end = eye + radius * glm::vec2( ray.x, ray.z );
                seenLo              = glm::min( seenLo, end );
                seenHi              = glm::max( seenHi, end );
            }
            const auto bulge =
                 static_cast<float>( radius * ( 1.0 - 1.0 / std::sqrt( 1.0 + tanX * tanX + tanY * tanY ) ) );
            lo = glm::max( lo, seenLo - ( bulge + pad ) );
            hi = glm::min( hi, seenHi + ( bulge + pad ) );
        }
        gpu.Cells = glm::ivec4( static_cast<int32_t>( std::floor( lo.x / cell ) ) - 1,
                                static_cast<int32_t>( std::floor( lo.y / cell ) ) - 1,
                                static_cast<int32_t>( std::ceil( ( hi.x - lo.x ) / cell ) ) + 2,
                                static_cast<int32_t>( std::ceil( ( hi.y - lo.y ) / cell ) ) + 2 );

        // What this frame's generation would read (VarietyGpu::FrameKey); AddFrame adds the field's revision.
        static_assert( sizeof( GrassParamsGpu ) % sizeof( uint32_t ) == 0 );
        gpu.FrameKey.assign( sizeof( GrassParamsGpu ) / sizeof( uint32_t ), 0u );
        std::memcpy( gpu.FrameKey.data(), &params, sizeof( GrassParamsGpu ) );
        for ( int i = 0; i < 4; ++i )
            gpu.FrameKey.push_back( static_cast<uint32_t>( gpu.Cells[i] ) );
        gpu.FrameKey.push_back( gpu.Capacity );
        for ( const void* buffer :
              { static_cast<const void*>( gpu.Instances.get() ), static_cast<const void*>( gpu.Args.get() ) } )
        {
            const auto address = reinterpret_cast<std::uintptr_t>( buffer ); // NOLINT: identity, not access
            gpu.FrameKey.push_back( static_cast<uint32_t>( address ) );
            gpu.FrameKey.push_back( static_cast<uint32_t>( static_cast<uint64_t>( address ) >> 32u ) );
        }
        return &gpu;
    }

    void GrassRenderer::AddFrame( RDG::Builder& graph, const ViewFrame& frame, MeshRenderer& meshes,
                                  const bool rayTracing, const GrassOcclusion& occlusion )
    {
        const bool testsHzb = occlusion.Hzb.IsValid();
        // The two volumes (GRASS-VIS-6): the camera's side planes, and the same planes swept along every light
        // whose cascades this frame fitted (UpdateCascades ran before this) for the full list the cascades and
        // the TLAS draw. While they differ the camera keeps a list of its own even with no HZB to test.
        ViewVolumes volumes;
        SidePlanes( frame.ViewProjection, volumes.Camera );
        std::copy( std::begin( volumes.Camera ), std::end( volumes.Camera ), std::begin( volumes.Full ) );
        volumes.Swept = SweepPlanes( volumes.Full, meshes.DirectionalCasterSweeps() );
        volumes.List  = testsHzb ? CameraList::PlanesAndHzb : volumes.Swept ? CameraList::Planes : CameraList::None;
        m_Frame.clear();
        m_FrameGenerate.clear();
        m_Settling       = false;
        m_FrameTested    = testsHzb;
        m_FrameNeutral   = occlusion.Neutral;
        m_VisibilityRows = 0;
        m_BladeRayTracing.BeginFrame( rayTracing );
        if ( m_Queue.empty() || !m_Generate || !m_Finalize )
            return;

        for ( const GrassFieldDraw& field : m_Queue )
        {
            if ( !field.Field || field.Field->Words.size() < 8 )
                continue;
            FieldGpu& fieldGpu = m_Fields[field.Root];
            if ( !fieldGpu.Buffer || fieldGpu.Revision != field.Field->Revision )
            {
                const auto bytes = static_cast<uint32_t>( field.Field->Words.size() * sizeof( uint32_t ) );
                fieldGpu.Buffer  = ShaderResources::StorageBuffer::Create( "GrassField", bytes, 0, true );
                if ( const auto wrote = fieldGpu.Buffer->SetData( field.Field->Words.data(), bytes );
                     !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the landscape's grass field was not uploaded: {}", wrote.GetError() );
                    fieldGpu.Buffer.reset();
                    continue;
                }
                fieldGpu.Revision = field.Field->Revision;
            }
            if ( const auto imported = Renderer::ImportBuffer( fieldGpu.Buffer, fieldGpu.Import ); !imported )
            {
                LOG_ERROR( "[Grass] the grass field is not in the frame graph: {}", imported.GetError() );
                continue;
            }
            fieldGpu.Ref = graph.RegisterExternal( fieldGpu.Import, "GrassField" );

            for ( const GrassVarietyDraw& variety : field.Varieties )
            {
                Desert::Mesh* mesh = variety.Blade ? BladeStrip( variety.Blade->Segments ) : variety.Mesh;
                if ( mesh == nullptr || variety.Sections.empty() || !mesh->GetGeometry().IsResident() ||
                     mesh->GetGeometry().IndexCount == 0 )
                    continue;
                VarietyGpu* gpu = PrepareVariety( variety, *mesh, frame, volumes );
                if ( gpu == nullptr )
                    continue;
                Common::BoolResultStr imported = Renderer::ImportBuffer( gpu->Instances, gpu->InstancesImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( gpu->Args, gpu->ArgsImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( gpu->Params, gpu->ParamsImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( gpu->Feedback, gpu->FeedbackImport );
                for ( const auto& [buffer, into] :
                      { std::pair{ &gpu->CameraInstances, &gpu->CameraInstancesImport },
                        std::pair{ &gpu->CameraArgs, &gpu->CameraArgsImport },
                        std::pair{ &gpu->Candidates, &gpu->CandidatesImport },
                        std::pair{ &gpu->PostArgs, &gpu->PostArgsImport } } )
                    if ( imported )
                        imported = Renderer::ImportBuffer( *buffer, *into );
                if ( !imported )
                {
                    LOG_ERROR( "[Grass] a variety's buffers are not in the frame graph: {}", imported.GetError() );
                    continue;
                }
                gpu->Refs.Instances = graph.RegisterExternal( gpu->InstancesImport, "GrassInstances" );
                gpu->Refs.Args      = graph.RegisterExternal( gpu->ArgsImport, "GrassArgs" );
                gpu->ParamsRef      = graph.RegisterExternal( gpu->ParamsImport, "GrassParams" );
                gpu->FeedbackRef    = graph.RegisterExternal( gpu->FeedbackImport, "GrassFeedback" );
                gpu->CameraRefs.Instances = graph.RegisterExternal( gpu->CameraInstancesImport, "GrassCameraInstances" );
                gpu->CameraRefs.Args      = graph.RegisterExternal( gpu->CameraArgsImport, "GrassCameraArgs" );
                gpu->CandidatesRef        = graph.RegisterExternal( gpu->CandidatesImport, "GrassCandidates" );
                gpu->PostRefs.Instances   = gpu->CameraRefs.Instances;
                gpu->PostRefs.Args        = graph.RegisterExternal( gpu->PostArgsImport, "GrassPostArgs" );
                gpu->FieldRef       = fieldGpu.Ref;
                m_Frame.push_back( gpu );
                if ( gpu->Drawn )
                    DrawCounter::RecordGrass( gpu->Drawn->x, gpu->Drawn->y, gpu->Drawn->z );
                const auto revision = static_cast<uint64_t>( fieldGpu.Revision );
                gpu->FrameKey.push_back( static_cast<uint32_t>( revision ) );
                gpu->FrameKey.push_back( static_cast<uint32_t>( revision >> 32u ) );
                gpu->FrameKey.push_back( static_cast<uint32_t>( volumes.List ) );
                if ( gpu->FrameKey != gpu->GenerateKey )
                {
                    gpu->GenerateKey       = gpu->FrameKey;
                    gpu->StableGenerations = 0;
                }
                // Instances culled by last frame's depth are this frame's only: a tested frame always generates.
                if ( testsHzb || gpu->StableGenerations < EngineContext::GetInstance().GetMaxFramesInFlight() )
                {
                    ++gpu->StableGenerations;
                    m_FrameGenerate.push_back( gpu );
                }
                NoteVarietyTextureDemand( variety, *mesh, *field.Field, frame );

                const bool cameraList = volumes.List != CameraList::None;
                std::vector<MeshRenderer::GpuInstancedRenderData> sections;
                sections.reserve( variety.Sections.size() );
                for ( uint32_t s = 0; s < variety.Sections.size(); ++s )
                    sections.push_back(
                         { .Mesh     = mesh,
                           .Path     = variety.Blade ? MeshVertexPath::GrassBlade : MeshVertexPath::Instanced,
                           .Material = variety.Sections[s].Material,
                           .HiddenSubmeshes  = variety.Sections[s].HiddenSubmeshes,
                           .CastShadows      = variety.Placement.CastDynamicShadow,
                           .Wind             = variety.Wind,
                           .Instances        = gpu->Instances,
                           .CameraInstances  = cameraList ? gpu->CameraInstances : nullptr,
                           .BladeShape       = gpu->Shape,
                           .Refs             = &gpu->Refs,
                           .CameraRefs       = cameraList ? &gpu->CameraRefs : nullptr,
                           .PostRefs         = testsHzb ? &gpu->PostRefs : nullptr,
                           .Capacity         = gpu->Capacity,
                           .BucketCapacities = gpu->BucketCapacities,
                           .Levels           = variety.Blade ? 1u : gpu->Buckets,
                           .Key              = gpu->Index * 64u + s } );
                // A Blade variety's levels in the view's TLAS: one shared BLAS per level, built from the shape
                // (GrassBladeRayTracing.hpp). A level a Masked section draws runs the alpha test, as MasksOf rules
                // for a mesh (RayTracingGather.cpp).
                if ( variety.Blade && m_BladeRayTracing.IsRequired() )
                {
                    uint64_t nonOpaque = 0;
                    for ( const MeshRenderer::GpuInstancedRenderData& section : sections )
                        if ( MeshRenderer::ShadingOf( section ).Masked )
                            nonOpaque |= ~section.HiddenSubmeshes;
                    const auto blas =
                         m_BladeRayTracing.PrepareVariety( graph, variety.Key, *mesh, gpu->ShapeData, nonOpaque );
                    if ( blas )
                        for ( MeshRenderer::GpuInstancedRenderData& section : sections )
                            section.BladeGeometry = blas.GetValue();
                    else
                        LOG_ERROR(
                             "[Grass] a blade variety is not in the view's TLAS; rays miss it this frame: {}",
                             blas.GetError() );
                }
                for ( const MeshRenderer::GpuInstancedRenderData& section : sections )
                    meshes.SubmitGpuInstanced( section );
            }
        }
        if ( m_Frame.empty() )
            return;

        // A frame whose varieties all keep their inputs generates nothing: the instances and the draw records
        // GrassFinalize left stay in their persistent buffers (VarietyGpu::GenerateKey).
        if ( !m_FrameGenerate.empty() )
        {
            GrassGeneratePush push;
            push.HzbViewProjection = occlusion.PrevViewProjection;
            // GrassGenerate.shader Hzb.w: 1 the camera's list by its planes and the HZB, 3 by its planes only, 0 one list.
            push.Hzb = testsHzb ? glm::uvec4( occlusion.Extent, 1u )
                                : glm::uvec4( 0u, 0u, 0u, volumes.List == CameraList::Planes ? 3u : 0u );
            const RDG::TextureRef hzb = testsHzb ? occlusion.Hzb : occlusion.Neutral;
            graph.AddPass(
                 "Grass: Generate", RDG::PassFlags::Compute,
                 [this, hzb]( RDG::PassBuilder& pass )
                 {
                     const auto& layout = m_GenerateLayout.Get( m_Generate->GetSpecification().Shader );
                     for ( const VarietyGpu* gpu : m_FrameGenerate )
                         pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Generate ) )
                              .Storage( GrassGenerateShaderParameters::Field, gpu->FieldRef,
                                        RDG::Access::StorageRead )
                              .Storage( GrassGenerateShaderParameters::Params, gpu->ParamsRef,
                                        RDG::Access::StorageRead )
                              .Storage( GrassGenerateShaderParameters::Instances, gpu->Refs.Instances,
                                        RDG::Access::StorageWrite )
                              .Storage( GrassGenerateShaderParameters::Args, gpu->Refs.Args,
                                        RDG::Access::StorageWrite )
                              .Sampler( GrassGenerateShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Texture( GrassGenerateShaderParameters::GrassHzbTexture, hzb,
                                        RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                              .Storage( GrassGenerateShaderParameters::CameraInstances, gpu->CameraRefs.Instances,
                                        RDG::Access::StorageWrite )
                              .Storage( GrassGenerateShaderParameters::CameraArgs, gpu->CameraRefs.Args,
                                        RDG::Access::StorageWrite )
                              .Storage( GrassGenerateShaderParameters::Candidates, gpu->CandidatesRef,
                                        RDG::Access::StorageWrite )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( GrassGeneratePush ) ) );
                 },
                 [this, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     auto& renderer = Renderer::GetInstance();
                     for ( uint32_t i = 0; i < m_FrameGenerate.size(); ++i )
                     {
                         const VarietyGpu* gpu = m_FrameGenerate[i];
                         RDG::PassBindings bindings( context, context.GetBindingBlock( i ) );
                         GrassGeneratePush variety = push;
                         variety.CellRange         = gpu->Cells;
                         bindings.PushConstants( &variety, sizeof( variety ) );
                         const auto groupsX =
                              ( static_cast<uint32_t>( gpu->Cells.z ) + kGrassGroupSize - 1 ) / kGrassGroupSize;
                         const auto groupsZ =
                              ( static_cast<uint32_t>( gpu->Cells.w ) + kGrassGroupSize - 1 ) / kGrassGroupSize;
                         if ( auto dispatched =
                                   renderer.DispatchCompute( bindings, *m_Generate, groupsX, groupsZ, 1 );
                              !dispatched )
                             return dispatched;
                     }
                     return BOOLSUCCESS;
                 } );

            graph.AddPass(
                 "Grass: Finalize", RDG::PassFlags::Compute,
                 [this]( RDG::PassBuilder& pass )
                 {
                     const auto& layout = m_FinalizeLayout.Get( m_Finalize->GetSpecification().Shader );
                     for ( const VarietyGpu* gpu : m_FrameGenerate )
                         pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Finalize ) )
                              .Storage( GrassFinalizeShaderParameters::Args, gpu->Refs.Args,
                                        RDG::Access::StorageWrite )
                              .Storage( GrassFinalizeShaderParameters::Feedback, gpu->FeedbackRef,
                                        RDG::Access::StorageWrite )
                              .Storage( GrassFinalizeShaderParameters::CameraArgs, gpu->CameraRefs.Args,
                                        RDG::Access::StorageWrite )
                              .Storage( GrassFinalizeShaderParameters::PostArgs, gpu->PostRefs.Args,
                                        RDG::Access::StorageWrite )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( GrassFinalizePush ) ) );
                 },
                 [this, cameraList = volumes.List != CameraList::None]( RDG::PassContext& context )
                      -> Common::BoolResultStr
                 {
                     auto& renderer = Renderer::GetInstance();
                     for ( uint32_t i = 0; i < m_FrameGenerate.size(); ++i )
                     {
                         RDG::PassBindings       bindings( context, context.GetBindingBlock( i ) );
                         const GrassFinalizePush mode{ glm::uvec4( 0u, cameraList ? 1u : 0u, 0u, 0u ) };
                         bindings.PushConstants( &mode, sizeof( mode ) );
                         if ( auto dispatched = renderer.DispatchCompute( bindings, *m_Finalize, 1, 1, 1 );
                              !dispatched )
                             return dispatched;
                     }
                     return BOOLSUCCESS;
                 } );
        }

        // The blade levels whose BLAS this frame builds: their vertices, before the view's TLAS block requests the
        // builds (SceneRenderer).
        if ( const Common::BoolResultStr blades = m_BladeRayTracing.AddPass( graph ); !blades )
            LOG_ERROR( "[Grass] the blade grass is not in the view's TLAS; rays miss it this frame: {}",
                       blades.GetError() );
    }

    bool GrassRenderer::AddFramePostOcclusion( RDG::Builder& graph, const RDG::TextureRef hzb,
                                               const glm::mat4& viewProjection, const glm::uvec3& extent )
    {
        // A tested frame generates every variety (AddFrame), so m_FrameGenerate is the frame's whole list.
        if ( !m_FrameTested || m_FrameGenerate.empty() || !m_Generate || !m_Finalize )
            return false;
        // No HZB of this frame (GrassGenerate.shader Hzb.z = 0): nothing to re-test against, so every candidate is
        // appended - drawn once more than an HZB would let through, never dropped.
        const bool             tests = hzb.IsValid();
        const RDG::TextureRef  bound = tests ? hzb : m_FrameNeutral;
        GrassGeneratePush push;
        push.HzbViewProjection = viewProjection;
        push.Hzb               = tests ? glm::uvec4( extent, 2u ) : glm::uvec4( 0u, 0u, 0u, 2u );
        graph.AddPass(
             tests ? "Grass: Post-Occlusion" : "Grass: Post-Occlusion (no HZB)", RDG::PassFlags::Compute,
             [this, bound]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_GenerateLayout.Get( m_Generate->GetSpecification().Shader );
                 for ( const VarietyGpu* gpu : m_FrameGenerate )
                     pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Generate ) )
                          .Storage( GrassGenerateShaderParameters::Field, gpu->FieldRef, RDG::Access::StorageRead )
                          .Storage( GrassGenerateShaderParameters::Params, gpu->ParamsRef, RDG::Access::StorageRead )
                          .Storage( GrassGenerateShaderParameters::Instances, gpu->Refs.Instances,
                                    RDG::Access::StorageRead )
                          .Storage( GrassGenerateShaderParameters::Args, gpu->Refs.Args, RDG::Access::StorageRead )
                          .Sampler( GrassGenerateShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                          .Texture( GrassGenerateShaderParameters::GrassHzbTexture, bound, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Storage( GrassGenerateShaderParameters::CameraInstances, gpu->CameraRefs.Instances,
                                    RDG::Access::StorageWrite )
                          .Storage( GrassGenerateShaderParameters::CameraArgs, gpu->CameraRefs.Args,
                                    RDG::Access::StorageWrite )
                          .Storage( GrassGenerateShaderParameters::Candidates, gpu->CandidatesRef,
                                    RDG::Access::StorageRead )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( GrassGeneratePush ) ) );
             },
             [this, push]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto& renderer = Renderer::GetInstance();
                 for ( uint32_t i = 0; i < m_FrameGenerate.size(); ++i )
                 {
                     const VarietyGpu* gpu = m_FrameGenerate[i];
                     // One thread per instance slot of the camera's list: a group is 64 threads (8 x 8).
                     const uint32_t groups = ( std::max( gpu->Capacity, 1u ) + 63u ) / 64u;
                     const uint32_t groupsX = std::min( groups, kPostGroupsX );
                     const uint32_t groupsY = ( groups + groupsX - 1u ) / groupsX;
                     RDG::PassBindings bindings( context, context.GetBindingBlock( i ) );
                     GrassGeneratePush variety = push;
                     variety.CellRange = glm::ivec4( static_cast<int32_t>( gpu->Capacity ),
                                                     static_cast<int32_t>( groupsX ), 0, 0 );
                     bindings.PushConstants( &variety, sizeof( variety ) );
                     if ( auto dispatched = renderer.DispatchCompute( bindings, *m_Generate, groupsX, groupsY, 1 );
                          !dispatched )
                         return dispatched;
                 }
                 return BOOLSUCCESS;
             } );

        graph.AddPass(
             "Grass: Post-Occlusion Finalize", RDG::PassFlags::Compute,
             [this]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_FinalizeLayout.Get( m_Finalize->GetSpecification().Shader );
                 for ( const VarietyGpu* gpu : m_FrameGenerate )
                     pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Finalize ) )
                          .Storage( GrassFinalizeShaderParameters::Args, gpu->Refs.Args, RDG::Access::StorageRead )
                          .Storage( GrassFinalizeShaderParameters::Feedback, gpu->FeedbackRef,
                                    RDG::Access::StorageWrite )
                          .Storage( GrassFinalizeShaderParameters::CameraArgs, gpu->CameraRefs.Args,
                                    RDG::Access::StorageRead )
                          .Storage( GrassFinalizeShaderParameters::PostArgs, gpu->PostRefs.Args,
                                    RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( GrassFinalizePush ) ) );
             },
             [this]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto& renderer = Renderer::GetInstance();
                 for ( uint32_t i = 0; i < m_FrameGenerate.size(); ++i )
                 {
                     RDG::PassBindings       bindings( context, context.GetBindingBlock( i ) );
                     const GrassFinalizePush mode{ glm::uvec4( 1u, 1u, 0u, 0u ) };
                     bindings.PushConstants( &mode, sizeof( mode ) );
                     if ( auto dispatched = renderer.DispatchCompute( bindings, *m_Finalize, 1, 1, 1 ); !dispatched )
                         return dispatched;
                 }
                 return BOOLSUCCESS;
             } );
        return true;
    }
} // namespace Desert::Graphic::System
