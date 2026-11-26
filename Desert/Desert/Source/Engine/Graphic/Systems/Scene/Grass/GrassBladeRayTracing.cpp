#include <Engine/Generated/ShaderPushConstants.gen.hpp>
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
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

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

    glm::vec4 GrassBladeHullLine( const glm::vec4* bands )
    {
        constexpr uint32_t kBands  = 8u;      // Assets::kGrassCardHullBands (GrassBladeShapeGpu::Hull)
        constexpr float    kNone   = -3.0f;   // below every side value (-1..1): no band meets this corner
        constexpr float    kMargin = 1.0e-4f; // the lines' rounding, in side units
        // One side's line: @p hiSide the hi edge, else the lo edge negated (so both are upper bounds). Its
        // corners at t = i / 8 hold the widest non-empty band meeting there; the line is the upper hull's edge
        // over t = 1/2 (least area: the trapezoid's mean width is its value there). False: no band passes.
        const auto line = [&]( const bool hiSide, float& atRoot, float& atTip ) -> bool
        {
            std::array<float, kBands + 1u> corner;
            corner.fill( kNone );
            float widest = kNone;
            for ( uint32_t b = 0u; b < kBands; ++b )
            {
                const glm::vec4& v  = bands[b / 2u];
                const float      lo = ( b & 1u ) == 0u ? v.x : v.z;
                const float      hi = ( b & 1u ) == 0u ? v.y : v.w;
                if ( lo > hi )
                    continue;
                const float e  = hiSide ? hi : -lo;
                corner[b]      = std::max( corner[b], e );
                corner[b + 1u] = std::max( corner[b + 1u], e );
                widest         = std::max( widest, e );
            }
            if ( widest <= kNone )
                return false;
            atRoot = widest; // the rectangle, unless a hull edge spans t = 1/2 inside [-1, 1]
            atTip  = widest;
            std::array<glm::vec2, kBands + 1u> hull{};
            size_t                             n = 0u;
            for ( uint32_t i = 0u; i <= kBands; ++i )
            {
                if ( corner[i] <= kNone )
                    continue;
                const glm::vec2 p( static_cast<float>( i ) / static_cast<float>( kBands ), corner[i] );
                // Drop the last corner while it lies on or under the chord from the one before it to p.
                while ( n >= 2u && ( hull[n - 1u].x - hull[n - 2u].x ) * ( p.y - hull[n - 2u].y ) -
                                            ( hull[n - 1u].y - hull[n - 2u].y ) * ( p.x - hull[n - 2u].x ) >=
                                       0.0f )
                    --n;
                hull[n++] = p;
            }
            for ( size_t k = 0u; k + 1u < n; ++k )
            {
                const glm::vec2 a = hull[k];
                const glm::vec2 c = hull[k + 1u];
                if ( a.x > 0.5f || c.x < 0.5f )
                    continue;
                const float slope = ( c.y - a.y ) / ( c.x - a.x );
                const float root  = a.y - slope * a.x;
                const float tip   = root + slope;
                if ( root >= -1.0f && root <= 1.0f && tip >= -1.0f && tip <= 1.0f )
                {
                    atRoot = std::min( root + kMargin, 1.0f );
                    atTip  = std::min( tip + kMargin, 1.0f );
                }
                break;
            }
            return true;
        };
        float hiRoot = 0.0f, hiTip = 0.0f, loRoot = 0.0f, loTip = 0.0f;
        if ( !line( true, hiRoot, hiTip ) || !line( false, loRoot, loTip ) )
            return glm::vec4( 2.0f, -2.0f, 2.0f, -2.0f );
        loRoot = -loRoot;
        loTip  = -loTip;
        // Sides that cross before an end (the bands stop short of it) meet there: the lo line only lowers, the hi
        // line only rises, so both still hold every band.
        const auto meet = []( float& lo, float& hi )
        {
            if ( lo > hi )
                lo = hi = 0.5f * ( lo + hi );
        };
        meet( loRoot, hiRoot );
        meet( loTip, hiTip );
        return glm::vec4( loRoot, hiRoot, loTip, hiTip );
    }

    GrassBladeStripLevel MakeGrassBladeStripLevel( const uint32_t segments )
    {
        GrassBladeStripLevel level;
        for ( uint32_t row = 0; row <= segments; ++row )
        {
            const float t = static_cast<float>( row ) / static_cast<float>( segments );
            for ( const float side : { -1.0f, 1.0f } )
                // z: the level's segment share, the raster's hull row span (Vertex_GrassBlade.glslh, FPS-GRASS-HULL).
                level.Vertices.push_back( { .Position  = glm::vec3( side, t, 1.0f / static_cast<float>( segments ) ),
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
                                          const GrassBladeShapeGpu& shape, const uint64_t nonOpaque,
                                          const bool pressed )
    {
        using Value = std::span<const RayTracing::RayTracingGeometryResource* const>;
        if ( !m_Required )
            return Common::MakeFormattedError<Value>( "{}: variety {} prepared on a frame that traces no ray",
                                                      kGrassBladeRayTracingVerticesPass, key );
        if ( const Common::BoolResultStr pipeline = EnsurePipeline(); !pipeline )
            return Common::MakeError<Value>( pipeline.GetError() );

        const std::vector<Submesh>& levels = strip.GetSubmeshes();
        const BladeKey              blade{ .Size      = shape.Blade.Size,
                                           .Bend      = shape.Blade.Bend,
                                           .Levels    = static_cast<uint32_t>( levels.size() ),
                                           .NonOpaque = nonOpaque };
        VarietyState&               state = m_Varieties[key];
        state.Seen                        = true;
        // Step 0 (the standing blade) at [0, levels), pressed step k at [k * levels, (k + 1) * levels).
        // RT-PRESS-LAZY: the pressed set only while @p pressed - created, built and registered the first frame a
        // press is live, then kept: a frame with no press neither builds nor registers nor traces it.
        constexpr uint32_t kSteps = RayTracing::kRayTracingBladePressSteps;
        if ( state.Levels != levels.size() )
        {
            state.Levels = levels.size();
            state.Sets   = {};
            state.Targets.clear();
            state.Geometry.clear();
        }
        const std::size_t targets = levels.size() * ( pressed ? 1u + kSteps : 1u );
        if ( state.Targets.size() < targets )
        {
            state.Targets.resize( targets );
            state.Geometry.resize( targets );
            for ( std::size_t t = 0; t < targets; ++t )
            {
                if ( !state.Targets[t] )
                    state.Targets[t] = RayTracing::CreateRayTracingDynamicMeshGeometry( "GrassBlade" );
                state.Geometry[t] = state.Targets[t].get();
            }
        }
        // A set holds its Key only once the graph that built it ended (IsBuilt): a build still pending (its graph
        // abandoned, or never recorded after an error) is built again, never read as storage no submitted build
        // wrote.
        const std::array<std::size_t, 3> bounds{ 0u, levels.size(), targets };
        std::array<bool, 2>               same{ true, true };
        for ( std::size_t s = 0; s + 1 < bounds.size() && bounds[s] < bounds[s + 1]; ++s )
        {
            same[s] = state.Sets[s].Valid && state.Sets[s].Key == blade;
            for ( std::size_t t = bounds[s]; t < bounds[s + 1]; ++t )
                same[s] = same[s] && state.Targets[t]->IsBuilt();
        }

        RDG::BufferRef shapeRef;
        if ( !same[0] || !same[1] )
        {
            shapeRef = graph.CreateBuffer( RDG::BufferDesc{ sizeof( GrassBladeShapeGpu ) }, "Grass.Blade.Shape" );
            graph.QueueBufferUpload( shapeRef, std::as_bytes( std::span( &shape, 1 ) ) );
        }
        for ( std::size_t t = 0; t < targets; ++t )
        {
            const std::size_t                                b       = t % levels.size();
            const uint32_t                                   step    = static_cast<uint32_t>( t / levels.size() );
            const std::size_t                                set     = step == 0u ? 0u : 1u;
            RayTracing::RayTracingDynamicMeshGeometryTarget& target  = *state.Targets[t];
            const RDG::BufferRef                             storage = graph.RegisterExternal(
                 target.GetStorage(), std::format( "Grass.Blade.RayTracing.BLAS.{}.{}", key, t ) );
            if ( same[set] )
            {
                m_Resident.push_back( storage );
                continue;
            }

            // The level as the raster draws it: the strip's submesh b is this level (GrassRenderer::BladeStrip).
            const Submesh&             submesh = levels[b];
            const GrassBladeStripLevel level   = MakeGrassBladeStripLevel( submesh.IndexCount / 6u );
            if ( level.Vertices.size() != submesh.VertexCount || level.Indices.size() * 3u != submesh.IndexCount )
            {
                state.Sets[set].Valid = false;
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
                state.Sets[set].Valid = false;
                return Common::MakeFormattedError<Value>(
                     "{}: variety {} level {} step {}: {}", kGrassBladeRayTracingVerticesPass, key, b, step,
                 plan.GetError() );
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
                 target.GetVertices(), std::format( "Grass.Blade.RayTracing.Vertices.{}.{}", key, t ) );
            job.Indices = graph.RegisterExternal( target.GetIndices(),
                                                  std::format( "Grass.Blade.RayTracing.Indices.{}.{}", key, t ) );
            job.Storage = storage;
            job.Counts  = glm::uvec4( static_cast<uint32_t>( level.Vertices.size() ),
                                      static_cast<uint32_t>( level.Indices.size() * 3u ), step, kSteps );
        }
        for ( std::size_t s = 0; s < same.size(); ++s )
            if ( !same[s] )
            {
                state.Sets[s].Key   = blade;
                state.Sets[s].Valid = true;
            }
        return Common::MakeSuccess( Value( state.Geometry ).first( targets ) );
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
                          .PushConstants<ShaderPush::GrassBladeRayTracingVertices>();
             },
             [pipeline = m_Pipeline, counts]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto& renderer = Renderer::GetInstance();
                 for ( uint32_t i = 0; i < counts.size(); ++i )
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( i ) );
                     bindings.PushConstants( ShaderPush::GrassBladeRayTracingVertices{ .Counts = counts[i] } );
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
