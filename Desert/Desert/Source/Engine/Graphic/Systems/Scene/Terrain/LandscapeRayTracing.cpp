#include <Engine/Graphic/Systems/Scene/Terrain/LandscapeRayTracing.hpp>

#include <Common/Core/Core.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <cstddef>
#include <cstring>
#include <format>
#include <string>

namespace Desert::Graphic::RayTracing
{
    namespace
    {
        struct TileWork
        {
            RayTracingDynamicMeshGeometryTarget* Target = nullptr;
            LandscapeRayTracingGrid              Grid;
            RayTracingDynamicGeometryPlan        Plan;
            uint32_t                             Row = 0; // in this frame's row buffer
            RDG::BufferRef                     Vertices;
            RDG::BufferRef                     Indices;
            RDG::BufferRef                     Storage;
            std::shared_ptr<Image>             Heightmap;
            std::shared_ptr<Image>             Weightmap; // the fallback when the tile has none
            bool                               HasWeightmap = false;
            uint32_t                           Entity       = 0;
        };
    } // namespace

    RayTracingDynamicMeshLayout LandscapeRayTracingLayout( const LandscapeRayTracingGrid& grid )
    {
        RayTracingDynamicMeshLayout layout;
        layout.VertexBytes = static_cast<uint64_t>( grid.GridVertices + 1u ) * sizeof( Vertex );
        layout.IndexBytes  = 2ull * grid.Triangles * 3u * sizeof( uint32_t );
        for ( uint32_t geometry = 0; geometry < kLandscapeRayTracingSegments; ++geometry )
        {
            RayTracingGeometrySegment segment;
            segment.VertexFormat    = RayTracingVertexFormat::Float3;
            segment.VertexStride    = static_cast<uint32_t>( sizeof( Vertex ) );
            segment.PositionOffset  = static_cast<uint32_t>( offsetof( Vertex, Position ) );
            segment.FirstVertex     = 0;
            segment.MaxVertex       = grid.GridVertices; // the NaN vertex is the last one
            segment.IndexType       = RayTracingIndexType::UInt32;
            segment.IndexByteOffset = geometry * grid.Triangles * 3u * static_cast<uint32_t>( sizeof( uint32_t ) );
            segment.PrimitiveCount  = grid.Triangles;
            segment.SubmeshIndex    = geometry;
            segment.Opaque          = IsLandscapeSegmentOpaque( geometry );
            layout.Segments.push_back( segment );
        }
        return layout;
    }

    LandscapeRayTracing::LandscapeRayTracing()  = default;
    LandscapeRayTracing::~LandscapeRayTracing() = default;

    Common::BoolResultStr LandscapeRayTracing::EnsurePipeline()
    {
        if ( m_Pipeline )
            return BOOLSUCCESS;
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        const auto shader  = shaders ? shaders->GetByName( std::string( kLandscapeRayTracingShader ) ) : nullptr;
        if ( !shader )
            return Common::MakeFormattedError<bool>(
                 "{}: compute shader '{}' is not registered (Programs/Terrain/{}.shader)",
                 kLandscapeRayTracingVerticesPass, kLandscapeRayTracingShader, kLandscapeRayTracingShader );
        const auto pipeline = ComputePipeline::Create(
             { .Shader = shader, .DebugName = std::string( kLandscapeRayTracingShader ) } );
        if ( !pipeline )
            return Common::MakeFormattedError<bool>( "{}: {}", kLandscapeRayTracingVerticesPass,
                                                     pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr LandscapeRayTracing::AddPasses( RDG::Builder& graph, FrameTextures& textures,
                                                          const bool                               required,
                                                          std::span<const LandscapeRayTracingTile> tiles )
    {
        m_Frame = {};
        if ( !required || tiles.empty() )
        {
            m_Tiles.clear();
            return BOOLSUCCESS;
        }
        if ( const Common::BoolResultStr pipeline = EnsurePipeline(); !pipeline )
            return pipeline;

        // Tiles the view no longer draws retire (their API objects go through the frame ring).
        std::unordered_map<uint32_t, TileState> kept;
        kept.reserve( tiles.size() );
        for ( const LandscapeRayTracingTile& tile : tiles )
        {
            auto found = m_Tiles.find( tile.Entity );
            if ( found != m_Tiles.end() )
                kept.emplace( tile.Entity, std::move( found->second ) );
        }
        m_Tiles = std::move( kept );

        std::vector<TileWork>                work;
        std::vector<System::TerrainInstance> rows;
        for ( const LandscapeRayTracingTile& tile : tiles )
        {
            if ( !tile.Heightmap )
                return Common::MakeFormattedError<bool>( "{}: tile entity {} has no heightmap",
                                                         kLandscapeRayTracingVerticesPass, tile.Entity );
            const System::TerrainInstance& instance = tile.Instance;
            const auto                     quads    = static_cast<uint32_t>( instance.LandscapeTile.z + 0.5f );
            const auto                     gridLod  = static_cast<uint32_t>( instance.Params.w + 0.5f );
            const BuildKey                 key{ .Heightmap       = tile.Heightmap.get(),
                                                .HeightRevision  = tile.HeightRevision,
                                                .Weightmap       = tile.Weightmap.get(),
                                                .WeightRevision  = tile.WeightRevision,
                                                .VisibilityLayer = tile.Weights.VisibilityLayer,
                                                .QuadsPerTile    = quads,
                                                .GridLod         = gridLod,
                                                .NeighbourMask = static_cast<uint32_t>( instance.Params2.w + 0.5f ) };

            TileState& state = m_Tiles[tile.Entity];
            if ( !state.Target )
                state.Target = CreateRayTracingDynamicMeshGeometry( "Landscape" );
            const bool sameBuild = state.Valid && state.Key == key;
            const bool sameRow =
                 sameBuild && std::memcmp( &state.Instance, &instance, sizeof( System::TerrainInstance ) ) == 0;

            if ( !sameRow )
            {
                TileWork job;
                job.Target                                      = state.Target.get();
                job.Grid                                        = LandscapeRayTracingGridAt( quads, gridLod );
                const Common::ResultStr<RayTracingDynamicGeometryPlan> plan =
                     state.Target->Prepare( LandscapeRayTracingLayout( job.Grid ), sameBuild );
                if ( !plan )
                {
                    m_Tiles.erase( tile.Entity );
                    return Common::MakeFormattedError<bool>( "{}: tile entity {}: {}",
                                                             kLandscapeRayTracingVerticesPass, tile.Entity,
                                                             plan.GetError() );
                }
                job.Plan         = plan.GetValue();
                job.Row          = static_cast<uint32_t>( rows.size() );
                job.Entity       = tile.Entity;
                job.Heightmap    = tile.Heightmap;
                job.HasWeightmap = tile.Weightmap != nullptr;
                job.Weightmap =
                     job.HasWeightmap
                          ? std::shared_ptr<Image>( tile.Weightmap )
                          : FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F );
                rows.push_back( instance );
                work.push_back( std::move( job ) );
                state.Key      = key;
                state.Instance = instance;
                state.Valid    = true;
                if ( job.Plan.Update )
                    ++m_Frame.Refit;
                else
                    ++m_Frame.Built;
            }

            // Every tile's BLAS is an instance of the TLAS and is read by its build: a rebuilt one through its
            // dynamic-geometry request, an unchanged one as Resident.
            const RDG::BufferRef storage = graph.RegisterExternal(
                 state.Target->GetStorage(), std::format( "Landscape.RayTracing.BLAS.{}", tile.Entity ) );
            if ( !sameRow )
                work.back().Storage = storage;
            else
                m_Frame.Resident.push_back( storage );
            if ( m_Frame.SegmentRecords.empty() )
                for ( uint32_t geometry = 0; geometry < kLandscapeRayTracingSegments; ++geometry )
                    m_Frame.SegmentRecords.push_back(
                         { .SubmeshIndex = geometry, .Opaque = IsLandscapeSegmentOpaque( geometry ) } );
            m_Frame.Primitives.push_back(
                 { .Geometry           = static_cast<const RayTracingGeometryResource*>( state.Target.get() ),
                   .World              = glm::mat4( 1.0f ), // the vertices are written in world space
                   .GpuSceneRow        = tile.GpuSceneRow,
                   .CastShadows        = tile.CastShadows,
                   .FirstSegmentRecord = 0,
                   .SegmentRecordCount = kLandscapeRayTracingSegments,
                   .Kind               = RayTracingPrimitiveKind::Landscape } );
            m_Frame.Materials.push_back( { .GpuSceneRow = tile.GpuSceneRow, .Weights = tile.Weights } );
            m_Frame.VertexBytes += state.Target->GetBuildInputBytes();
            m_Frame.BlasBytes += state.Target->GetMemoryBytes();
        }
        if ( work.empty() )
            return BOOLSUCCESS;

        // The rows the vertex compute reads are the raster's own, uploaded for this pass.
        std::vector<std::byte> rowBytes( rows.size() * sizeof( System::TerrainInstance ) );
        std::memcpy( rowBytes.data(), rows.data(), rowBytes.size() );
        const RDG::BufferRef rowBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ rowBytes.size() }, "Landscape.RayTracing.Rows" );
        graph.QueueBufferUpload( rowBuffer, rowBytes );

        // ── "Landscape: Ray Tracing Vertices": one binding block and one dispatch per tile that changed
        // ──────────
        ComputeNodeDeclaration vertices;
        vertices.Name      = std::string( kLandscapeRayTracingVerticesPass );
        const auto& layout = m_Layout.Get( m_Pipeline->GetSpecification().Shader );
        for ( TileWork& job : work )
        {
            job.Vertices = graph.RegisterExternal( job.Target->GetVertices(),
                                                   std::format( "Landscape.RayTracing.Vertices.{}", job.Entity ) );
            job.Indices  = graph.RegisterExternal( job.Target->GetIndices(),
                                                   std::format( "Landscape.RayTracing.Indices.{}", job.Entity ) );
            vertices.Access.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) )
                 .Storage( "LandscapeRayTracingRows", rowBuffer, RDG::Access::StorageRead )
                 .Sampled( "u_Heightmap", job.Heightmap, RDG::Access::SampledCompute, RDG::SamplerDesc{},
                           std::format( "Landscape.Heightmap.{}", job.Entity ) )
                 .Sampled( "u_Weightmap", job.Weightmap, RDG::Access::SampledCompute, RDG::SamplerDesc{},
                           job.HasWeightmap ? std::format( "Landscape.Weightmap.{}", job.Entity )
                                            : std::string( "Landscape.RayTracing.WeightmapFallback" ) )
                 .Storage( "LandscapeRayTracingVertexData", job.Vertices, RDG::Access::StorageWrite )
                 .Storage( "LandscapeRayTracingIndexData", job.Indices, RDG::Access::StorageWrite );
        }
        std::vector<glm::uvec4> pushes;
        std::vector<uint32_t>   groups;
        for ( const TileWork& job : work )
        {
            pushes.emplace_back( job.Row, job.Grid.Side, job.Grid.Cells, job.HasWeightmap ? 1u : 0u );
            const uint32_t invocations = std::max( job.Grid.GridVertices, job.Grid.Cells * job.Grid.Cells );
            groups.push_back( ( invocations + kLandscapeRayTracingLocalSize - 1 ) /
                              kLandscapeRayTracingLocalSize );
        }
        vertices.Record = [pipeline = m_Pipeline, pushes, groups]( RDG::PassContext& context,
                                                                   const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            for ( uint32_t block = 0; block < pushes.size(); ++block )
            {
                RDG::PassBindings bindings( context, context.GetBindingBlock( block ) );
                bindings.PushConstants( &pushes[block], static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                if ( const Common::BoolResultStr dispatched =
                          Renderer::DispatchCompute( bindings, *pipeline, groups[block], 1, 1 );
                     !dispatched )
                    return dispatched;
            }
            return BOOLSUCCESS;
        };
        std::vector<ComputeNodeDeclaration> nodes;
        nodes.push_back( std::move( vertices ) );
        if ( !AddComputeNodes( graph, textures, std::move( nodes ) ) )
            return Common::MakeFormattedError<bool>( "{}: the node could not be declared",
                                                     kLandscapeRayTracingVerticesPass );

        // Each changed tile's BLAS: a request of the RT scene's one dynamic-geometry pass, declared after this node.
        m_Frame.Builds.reserve( work.size() );
        for ( const TileWork& job : work )
            m_Frame.Builds.push_back( { .Target       = job.Target,
                                        .Vertices     = job.Vertices,
                                        .Indices      = job.Indices,
                                        .Storage      = job.Storage,
                                        .ScratchBytes = job.Plan.ScratchBytes } );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
