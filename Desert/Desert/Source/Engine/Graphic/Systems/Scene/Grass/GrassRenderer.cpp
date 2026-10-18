// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Private/LandscapeGrass.cpp:2032-2080 (FGrassBuilderBase: the
// grid size from GrassDensity) and :2378-2520 (FAsyncGrassBuilder::Build, its grid branch), adapted: the instances
// are generated on the GPU every frame on a world-aligned grid around the view
// (Programs/Grass/GrassGenerate.shader) instead of being built on worker threads per landscape component into HISM
// tiles, and they are drawn by indirect draws through the mesh renderer's instanced cells (GpuInstances.hpp)
// instead of a HISM component.
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGrassGroupSize       = 8; // GrassGenerate's LocalSize
        constexpr uint32_t kFlagRandomRotation   = 1u;
        constexpr uint32_t kFlagAlignToSurface   = 2u;
        constexpr uint32_t kFlagWeightAttenuates = 4u;

        // Mirrors GrassGenerate's GrassParams (std430).
        struct GrassParamsGpu
        {
            glm::vec4  Planes[4];
            glm::vec4  Camera;
            glm::vec4  Cull;
            glm::vec4  Range;
            glm::vec4  ScaleX;
            glm::vec4  ScaleY;
            glm::vec4  ScaleZ;
            glm::uvec4 Info;
        };
        static_assert( sizeof( GrassParamsGpu ) == 176 );

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

    GrassRenderer::VarietyGpu* GrassRenderer::PrepareVariety( const GrassVarietyDraw& variety,
                                                              const ViewFrame&        frame )
    {
        const GrassPlacement& p      = variety.Placement;
        const float           cell   = CellSizeCm( p );
        const float           radius = p.EndCullDistance;
        // Every cell whose jittered instance can lie within the cull radius: the disk widened by one cell
        // diagonal.
        const double   reach = static_cast<double>( radius ) + 1.5 * static_cast<double>( cell );
        const uint32_t capacity =
             static_cast<uint32_t>( std::ceil( 3.14159265 * reach * reach / ( cell * cell ) ) );

        VarietyGpu& gpu       = m_Varieties[variety.Key];
        const auto& submeshes = variety.Mesh->GetSubmeshes();
        if ( gpu.Mesh != variety.Mesh || gpu.Capacity != capacity || !gpu.Instances )
        {
            gpu.Mesh     = variety.Mesh;
            gpu.Capacity = capacity;
            if ( gpu.Index == 0 )
                gpu.Index = ++m_NextIndex;
            gpu.Instances = ShaderResources::StorageBuffer::Create( "GrassInstances",
                                                                    capacity * sizeof( glm::mat4 ), 2, true );
            const auto argBytes =
                 static_cast<uint32_t>( GpuInstanceArgsOffset( static_cast<uint32_t>( submeshes.size() ) ) );
            gpu.Args = ShaderResources::StorageBuffer::Create( "GrassArgs", argBytes, 3, true );
            // The records' static half: each submesh's level-0 index range; instanceCount is GrassFinalize's.
            std::vector<uint32_t> args( argBytes / 4u, 0u );
            args[1] = capacity;
            args[2] = static_cast<uint32_t>( submeshes.size() );
            for ( size_t si = 0; si < submeshes.size(); ++si )
            {
                const auto& sm     = submeshes[si];
                uint32_t*   record = &args[GpuInstanceArgsOffset( static_cast<uint32_t>( si ) ) / 4u];
                record[0]          = sm.LODs.empty() ? sm.IndexCount : sm.LODs[0].IndexCount;
                record[2]          = sm.LODs.empty() ? sm.IndexOffset : sm.LODs[0].IndexOffset;
                record[3]          = sm.VertexOffset;
            }
            if ( const auto wrote = gpu.Args->SetData( args.data(), argBytes ); !wrote.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the argument records were not written: {}", wrote.GetError() );
                return nullptr;
            }
            gpu.Params = ShaderResources::StorageBuffer::Create( "GrassParams", sizeof( GrassParamsGpu ), 1 );
        }

        GrassParamsGpu params{};
        SidePlanes( frame.ViewProjection, params.Planes );
        params.Camera                   = glm::vec4( frame.CameraPosition, radius );
        params.Cull                     = glm::vec4( p.StartCullDistance, p.EndCullDistance, cell,
                                                     std::clamp( p.PlacementJitter, 0.0f, 0.99f ) * 0.5f );
        const Common::Math::AABB bounds = Geometry::LocalBounds( submeshes );
        const glm::vec3          centre = ( bounds.Min + bounds.Max ) * 0.5f;
        params.Range = glm::vec4( p.AllowedDensityRange.x, p.AllowedDensityRange.y, p.MaxScaleWeightAttenuation,
                                  glm::length( bounds.Max - bounds.Min ) * 0.5f +
                                       glm::length( glm::vec2( centre.x, centre.z ) ) );
        const uint32_t flags = ( p.RandomRotation ? kFlagRandomRotation : 0u ) |
                               ( p.AlignToSurface ? kFlagAlignToSurface : 0u ) |
                               ( p.WeightAttenuatesMaxScale ? kFlagWeightAttenuates : 0u );
        params.ScaleX = glm::vec4( p.ScaleX, static_cast<float>( p.Scaling ), std::bit_cast<float>( flags ) );
        params.ScaleY = glm::vec4( p.ScaleY, centre.y, 0.0f );
        params.ScaleZ = glm::vec4( p.ScaleZ, 0.0f, 0.0f );
        params.Info   = glm::uvec4( variety.Layer, capacity, static_cast<uint32_t>( submeshes.size() ),
                                    static_cast<uint32_t>( variety.Key * 0x9E3779B97F4A7C15ull >> 32 ) );
        if ( const auto wrote = gpu.Params->SetData( &params, sizeof( params ) ); !wrote.IsSuccess() )
        {
            LOG_ERROR( "[Grass] the variety's parameters were not written: {}", wrote.GetError() );
            return nullptr;
        }

        const glm::vec2 lo    = glm::vec2( frame.CameraPosition.x, frame.CameraPosition.z ) - radius;
        const auto      cells = static_cast<int32_t>( std::ceil( 2.0f * radius / cell ) ) + 2;
        gpu.Cells             = glm::ivec4( static_cast<int32_t>( std::floor( lo.x / cell ) ) - 1,
                                            static_cast<int32_t>( std::floor( lo.y / cell ) ) - 1, cells, 0 );
        return &gpu;
    }

    void GrassRenderer::AddFrame( RDG::Builder& graph, const ViewFrame& frame, MeshRenderer& meshes )
    {
        m_Frame.clear();
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
                if ( variety.Mesh == nullptr || variety.Sections.empty() || !variety.Mesh->GetIndexBuffer() )
                    continue;
                VarietyGpu* gpu = PrepareVariety( variety, frame );
                if ( gpu == nullptr )
                    continue;
                Common::BoolResultStr imported = Renderer::ImportBuffer( gpu->Instances, gpu->InstancesImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( gpu->Args, gpu->ArgsImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( gpu->Params, gpu->ParamsImport );
                if ( !imported )
                {
                    LOG_ERROR( "[Grass] a variety's buffers are not in the frame graph: {}", imported.GetError() );
                    continue;
                }
                gpu->Refs.Instances = graph.RegisterExternal( gpu->InstancesImport, "GrassInstances" );
                gpu->Refs.Args      = graph.RegisterExternal( gpu->ArgsImport, "GrassArgs" );
                gpu->ParamsRef      = graph.RegisterExternal( gpu->ParamsImport, "GrassParams" );
                gpu->FieldRef       = fieldGpu.Ref;
                m_Frame.push_back( gpu );

                for ( uint32_t s = 0; s < variety.Sections.size(); ++s )
                    meshes.SubmitGpuInstanced( { .Mesh            = variety.Mesh,
                                                 .Material        = variety.Sections[s].Material,
                                                 .HiddenSubmeshes = variety.Sections[s].HiddenSubmeshes,
                                                 .CastShadows     = variety.Placement.CastDynamicShadow,
                                                 .Wind            = variety.Wind,
                                                 .Instances       = gpu->Instances,
                                                 .Refs            = &gpu->Refs,
                                                 .Key             = gpu->Index * 64u + s } );
            }
        }
        if ( m_Frame.empty() )
            return;

        graph.AddPass(
             "Grass: Generate", RDG::PassFlags::Compute,
             [this]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_GenerateLayout.Get( m_Generate->GetSpecification().Shader );
                 for ( const VarietyGpu* gpu : m_Frame )
                     pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Generate ) )
                          .Storage( "Field", gpu->FieldRef, RDG::Access::StorageRead )
                          .Storage( "Params", gpu->ParamsRef, RDG::Access::StorageRead )
                          .Storage( "Instances", gpu->Refs.Instances, RDG::Access::StorageWrite )
                          .Storage( "Args", gpu->Refs.Args, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::ivec4 ) ) );
             },
             [this]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto& renderer = Renderer::GetInstance();
                 for ( uint32_t i = 0; i < m_Frame.size(); ++i )
                 {
                     const VarietyGpu* gpu = m_Frame[i];
                     RDG::PassBindings bindings( context, context.GetBindingBlock( i ) );
                     bindings.PushConstants( &gpu->Cells, sizeof( gpu->Cells ) );
                     const auto groups =
                          ( static_cast<uint32_t>( gpu->Cells.z ) + kGrassGroupSize - 1 ) / kGrassGroupSize;
                     if ( auto dispatched = renderer.DispatchCompute( bindings, *m_Generate, groups, groups, 1 );
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
                 for ( const VarietyGpu* gpu : m_Frame )
                     pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Finalize ) )
                          .Storage( "Args", gpu->Refs.Args, RDG::Access::StorageWrite );
             },
             [this]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto& renderer = Renderer::GetInstance();
                 for ( uint32_t i = 0; i < m_Frame.size(); ++i )
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( i ) );
                     if ( auto dispatched = renderer.DispatchCompute( bindings, *m_Finalize, 1, 1, 1 );
                          !dispatched )
                         return dispatched;
                 }
                 return BOOLSUCCESS;
             } );
    }
} // namespace Desert::Graphic::System
