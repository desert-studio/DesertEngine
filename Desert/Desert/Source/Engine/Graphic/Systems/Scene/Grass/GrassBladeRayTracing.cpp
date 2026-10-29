#include <Engine/Graphic/Systems/Scene/Grass/GrassBladeRayTracing.hpp>

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr const char* kProgramName = "GrassBladeRayTracingVertices";

        // GrassBladeRayTracingVertices.shader's buffers: bound through these members, checked against the shader's
        // reflected layout when the pipeline is created.
        struct GrassBladeRayTracingVerticesShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, GrassBladeShape );
            DESERT_SHADER_PARAMETER( StorageBuffer, GrassBladeStripVertices );
            DESERT_SHADER_PARAMETER( StorageBuffer, GrassBladeStripIndices );
            DESERT_SHADER_PARAMETER( StorageBuffer, GrassBladeRayTracingVertexData );
            DESERT_SHADER_PARAMETER( StorageBuffer, GrassBladeRayTracingIndexData );
            static constexpr std::array kAll{ RDG::Erase( GrassBladeShape ), RDG::Erase( GrassBladeStripVertices ),
                                              RDG::Erase( GrassBladeStripIndices ),
                                              RDG::Erase( GrassBladeRayTracingVertexData ),
                                              RDG::Erase( GrassBladeRayTracingIndexData ) };
        };
        constexpr uint32_t    kLocalSize   = 64; // the program's LocalSize
    } // namespace

    GrassBladeStripLevel MakeGrassBladeStripLevel( const uint32_t segments )
    {
        GrassBladeStripLevel level;
        for ( uint32_t row = 0; row <= segments; ++row )
        {
            const float t = static_cast<float>( row ) / static_cast<float>( segments );
            for ( const float side : { -1.0f, 1.0f } )
                level.Vertices.push_back( { .Position  = glm::vec3( side, t, 0.0f ),
                                            .Normal    = glm::vec3( 0.0f, 0.0f, 1.0f ),
                                            .Tangent   = glm::vec3( 1.0f, 0.0f, 0.0f ),
                                            .Bitangent = glm::vec3( 0.0f, 1.0f, 0.0f ),
                                            .TexCoord  = glm::vec2( side * 0.5f + 0.5f, t ) } );
        }
        for ( uint32_t row = 0; row < segments; ++row )
        {
            const uint32_t a = row * 2u;
            level.Indices.push_back( { a, a + 1u, a + 2u } );
            level.Indices.push_back( { a + 1u, a + 3u, a + 2u } );
        }
        return level;
    }

    void GrassBladeRayTracing::BeginFrame( const bool required )
    {
        m_Required = required;
        m_Work.clear();
        m_Builds.clear();
        m_Resident.clear();
        if ( !required )
        {
            m_Varieties.clear();
            return;
        }
        // Varieties the last frame did not draw retire (their API objects go through the frame ring).
        std::erase_if( m_Varieties, []( const auto& entry ) { return !entry.second.Seen; } );
        for ( auto& [key, state] : m_Varieties )
            state.Seen = false;
    }

    Common::BoolResultStr GrassBladeRayTracing::EnsurePipeline()
    {
        if ( m_Pipeline )
            return BOOLSUCCESS;
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( kProgramName );
        if ( !shader )
            return Common::MakeError(
                 std::format( "{}: missing shader '{}'", kGrassBladeRayTracingVerticesPass, kProgramName ) );
        const auto made = ComputePipeline::Create( { .Shader = shader, .DebugName = kProgramName } );
        if ( !made )
            return Common::MakeError( made.GetError() );
        if ( auto checked = m_Layout.Validate( shader, GrassBladeRayTracingVerticesShaderParameters::kAll ); !checked )
            return Common::MakeError( std::format( "{}: {}", kGrassBladeRayTracingVerticesPass, checked.GetError() ) );
        m_Pipeline = made.GetValue();
        return BOOLSUCCESS;
    }

    Common::ResultStr<std::span<const RayTracing::RayTracingGeometryResource* const>>
    GrassBladeRayTracing::PrepareVariety( RDG::Builder& graph, const uint64_t key, const Desert::Mesh& strip,
                                          const GrassBladeShapeGpu& shape, const uint64_t nonOpaque )
    {
        using Value = std::span<const RayTracing::RayTracingGeometryResource* const>;
        if ( !m_Required )
            return Common::MakeFormattedError<Value>( "{}: variety {} prepared on a frame that traces no ray",
                                                      kGrassBladeRayTracingVerticesPass, key );
        if ( const Common::BoolResultStr pipeline = EnsurePipeline(); !pipeline )
            return Common::MakeError<Value>( pipeline.GetError() );

        const std::vector<Submesh>& levels = strip.GetSubmeshes();
        const BladeKey              blade{ .Size      = shape.Size,
                                           .Bend      = shape.Bend,
                                           .Levels    = static_cast<uint32_t>( levels.size() ),
                                           .NonOpaque = nonOpaque };
        VarietyState&               state = m_Varieties[key];
        state.Seen                        = true;
        const bool same                   = state.Valid && state.Key == blade;
        if ( state.Targets.size() != levels.size() )
        {
            state.Targets.resize( levels.size() );
            state.Geometry.resize( levels.size() );
            for ( std::size_t b = 0; b < levels.size(); ++b )
            {
                if ( !state.Targets[b] )
                    state.Targets[b] = RayTracing::CreateRayTracingDynamicMeshGeometry( "GrassBlade" );
                state.Geometry[b] = state.Targets[b].get();
            }
        }

        RDG::BufferRef shapeRef;
        if ( !same )
        {
            shapeRef = graph.CreateBuffer( RDG::BufferDesc{ sizeof( GrassBladeShapeGpu ) }, "Grass.Blade.Shape" );
            graph.QueueBufferUpload( shapeRef, std::as_bytes( std::span( &shape, 1 ) ) );
        }
        for ( std::size_t b = 0; b < levels.size(); ++b )
        {
            RayTracing::RayTracingDynamicMeshGeometryTarget& target  = *state.Targets[b];
            const RDG::BufferRef                             storage = graph.RegisterExternal(
                 target.GetStorage(), std::format( "Grass.Blade.RayTracing.BLAS.{}.{}", key, b ) );
            if ( same )
            {
                m_Resident.push_back( storage );
                continue;
            }

            // The level as the raster draws it: the strip's submesh b is this level (GrassRenderer::BladeStrip).
            const Submesh&             submesh = levels[b];
            const GrassBladeStripLevel level   = MakeGrassBladeStripLevel( submesh.IndexCount / 6u );
            if ( level.Vertices.size() != submesh.VertexCount || level.Indices.size() * 3u != submesh.IndexCount )
            {
                state.Valid = false;
                return Common::MakeFormattedError<Value>(
                     "{}: variety {} level {}: the strip's submesh has {} vertices and {} indices, its level {} "
                     "and {}",
                     kGrassBladeRayTracingVerticesPass, key, b, submesh.VertexCount, submesh.IndexCount,
                     level.Vertices.size(), level.Indices.size() * 3u );
            }

            RayTracing::RayTracingDynamicMeshLayout layout;
            layout.VertexBytes                             = level.Vertices.size() * sizeof( Vertex );
            layout.IndexBytes                              = level.Indices.size() * sizeof( Index );
            RayTracing::RayTracingGeometrySegment& segment = layout.Segments.emplace_back();
            segment.VertexFormat                           = RayTracing::RayTracingVertexFormat::Float3;
            segment.VertexStride                           = static_cast<uint32_t>( sizeof( Vertex ) );
            segment.PositionOffset                         = static_cast<uint32_t>( offsetof( Vertex, Position ) );
            segment.FirstVertex                            = 0;
            segment.MaxVertex                              = static_cast<uint32_t>( level.Vertices.size() - 1u );
            segment.IndexType                              = RayTracing::RayTracingIndexType::UInt32;
            segment.IndexByteOffset                        = 0;
            segment.PrimitiveCount                         = static_cast<uint32_t>( level.Indices.size() );
            segment.SubmeshIndex                           = static_cast<uint32_t>( b );
            segment.Opaque                                 = !RayTracing::IsSubmeshHidden( nonOpaque, b );
            const Common::ResultStr<RayTracing::RayTracingDynamicGeometryPlan> plan =
                 target.Prepare( layout, false );
            if ( !plan )
            {
                state.Valid = false;
                return Common::MakeFormattedError<Value>(
                     "{}: variety {} level {}: {}", kGrassBladeRayTracingVerticesPass, key, b, plan.GetError() );
            }

            LevelWork& job   = m_Work.emplace_back();
            job.Target       = &target;
            job.ScratchBytes = plan.GetValue().ScratchBytes;
            job.Shape        = shapeRef;
            job.StripVertices =
                 graph.CreateBuffer( RDG::BufferDesc{ layout.VertexBytes }, "Grass.Blade.StripVertices" );
            graph.QueueBufferUpload( job.StripVertices, std::as_bytes( std::span( level.Vertices ) ) );
            job.StripIndices =
                 graph.CreateBuffer( RDG::BufferDesc{ layout.IndexBytes }, "Grass.Blade.StripIndices" );
            graph.QueueBufferUpload( job.StripIndices, std::as_bytes( std::span( level.Indices ) ) );
            job.Vertices = graph.RegisterExternal(
                 target.GetVertices(), std::format( "Grass.Blade.RayTracing.Vertices.{}.{}", key, b ) );
            job.Indices = graph.RegisterExternal( target.GetIndices(),
                                                  std::format( "Grass.Blade.RayTracing.Indices.{}.{}", key, b ) );
            job.Storage = storage;
            job.Counts  = glm::uvec4( static_cast<uint32_t>( level.Vertices.size() ),
                                      static_cast<uint32_t>( level.Indices.size() * 3u ), 0u, 0u );
        }
        if ( !same )
        {
            state.Key   = blade;
            state.Valid = true;
        }
        return Common::MakeSuccess( Value( state.Geometry ) );
    }

    Common::BoolResultStr GrassBladeRayTracing::AddPass( RDG::Builder& graph )
    {
        if ( m_Work.empty() )
            return BOOLSUCCESS;

        std::vector<glm::uvec4> counts;
        counts.reserve( m_Work.size() );
        for ( const LevelWork& job : m_Work )
            counts.push_back( job.Counts );
        graph.AddPass(
             kGrassBladeRayTracingVerticesPass, RDG::PassFlags::Compute,
             [this]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_Layout.Get( m_Pipeline->GetSpecification().Shader );
                 for ( const LevelWork& job : m_Work )
                     pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                          .Storage( GrassBladeRayTracingVerticesShaderParameters::GrassBladeShape, job.Shape, RDG::Access::StorageRead )
                          .Storage( GrassBladeRayTracingVerticesShaderParameters::GrassBladeStripVertices, job.StripVertices, RDG::Access::StorageRead )
                          .Storage( GrassBladeRayTracingVerticesShaderParameters::GrassBladeStripIndices, job.StripIndices, RDG::Access::StorageRead )
                          .Storage( GrassBladeRayTracingVerticesShaderParameters::GrassBladeRayTracingVertexData, job.Vertices, RDG::Access::StorageWrite )
                          .Storage( GrassBladeRayTracingVerticesShaderParameters::GrassBladeRayTracingIndexData, job.Indices, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
             },
             [pipeline = m_Pipeline, counts]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto& renderer = Renderer::GetInstance();
                 for ( uint32_t i = 0; i < counts.size(); ++i )
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( i ) );
                     bindings.PushConstants( &counts[i], static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                     const uint32_t groups =
                          ( std::max( counts[i].x, counts[i].y ) + kLocalSize - 1 ) / kLocalSize;
                     if ( auto dispatched = renderer.DispatchCompute( bindings, *pipeline, groups, 1, 1 );
                          !dispatched )
                         return dispatched;
                 }
                 return BOOLSUCCESS;
             } );

        // Each changed level's BLAS: a request of the RT scene's one dynamic-geometry pass, declared after this
        // pass (the TLAS block follows the grass in SceneRenderer).
        m_Builds.reserve( m_Work.size() );
        for ( const LevelWork& job : m_Work )
            m_Builds.push_back( { .Target       = job.Target,
                                  .Vertices     = job.Vertices,
                                  .Indices      = job.Indices,
                                  .Storage      = job.Storage,
                                  .ScratchBytes = job.ScratchBytes } );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
