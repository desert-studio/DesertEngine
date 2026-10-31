#include <Engine/Graphic/VirtualGeometry/VisibilityBuffer.hpp>

#include <Engine/Graphic/GpuInstances.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <format>

namespace Desert::Graphic::VirtualGeometry
{
    namespace
    {
        // The slot name "VirtualGeometry: Visibility Clear" binds, spelled once (RDGShaderParameters.hpp).
        struct VisibilityClearShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Visibility );
        };
        // ... and the one "Grass: Visibility Depth" binds (VisibilityDepth.shader's fragment stage).
        struct VisibilityDepthShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Visibility );
        };

        // VisibilityClear.shader's LocalSize and the row of groups one dispatch row covers: a 4K view is ~130k
        // groups of 64, past the 65535 a dispatch dimension guarantees, so the groups are laid out 2D and the
        // shader rebuilds the pixel from gl_NumWorkGroups.x.
        inline constexpr uint32_t kClearGroupSize = 64u;
        inline constexpr uint32_t kClearGroupsRow = 1024u;
        // The resolve's argument table: the header and one record per submesh a blade strip can have (a mask of
        // 64 bits names them; GrassBladeStripLevel). Record 0 draws; the rest stay zero.
        inline constexpr uint32_t kResolveRecords = 64u;
        inline constexpr uint32_t kResolveArgBytes =
             static_cast<uint32_t>( GpuInstanceArgsOffset( kResolveRecords ) );
    } // namespace

    Common::BoolResultStr VisibilityBuffer::Initialize()
    {
        auto*      shaders = Runtime::ResourceRegistry::GetShaderService();
        const auto shader  = shaders != nullptr ? shaders->GetByName( "VisibilityClear" ) : nullptr;
        if ( !shader )
            return Common::MakeError( "VisibilityBuffer: missing shader 'VisibilityClear'" );
        const auto made = ComputePipeline::Create( { .Shader = shader, .DebugName = "VisibilityClear" } );
        if ( !made )
            return Common::MakeError( std::format( "VisibilityBuffer: {}", made.GetError() ) );
        m_Clear = made.GetValue();

        // "Grass: Visibility Depth": the G-buffer depth alone (single-sample DEPTH32F, the target of the raster
        // it follows), tested CloserOrEqual and written; no colour, no blending, both faces of the full-screen
        // triangle.
        const auto depthShader = shaders->GetByName( "VisibilityDepth" );
        if ( !depthShader )
            return Common::MakeError( "VisibilityBuffer: missing shader 'VisibilityDepth'" );
        GraphicsPipelineSpecification depth;
        depth.DebugName    = "GrassVisibilityDepth";
        depth.Shader       = depthShader;
        depth.TargetLayout = RenderTargetLayout{
             .ColorFormats = {}, .DepthFormat = ViewTargetFormats::kGBufferDepth, .Samples = 1 };
        depth.DepthTestEnabled  = true;
        depth.DepthCompareOp    = DepthCompare::CloserOrEqual;
        depth.DepthWriteEnabled = true;
        depth.CullMode          = CullMode::None;
        const auto depthMade    = GraphicsPipeline::Create( depth );
        if ( !depthMade )
            return Common::MakeError(
                 std::format( "VisibilityBuffer: the depth pass: {}", depthMade.GetError() ) );
        m_Depth = depthMade.GetValue();

        m_ResolveArgs =
             ShaderResources::StorageBuffer::Create( "VisibilityResolveArgs", kResolveArgBytes, 0, true );
        if ( !m_ResolveArgs )
            return Common::MakeError( "VisibilityBuffer: the resolve's argument table was not created" );
        std::array<uint32_t, kResolveArgBytes / 4u> words{};
        // Header: one bucket, one instance of capacity, kResolveRecords submeshes, last drawn count 1.
        words[0] = 1u;
        words[1] = 1u;
        words[2] = kResolveRecords;
        words[3] = 1u;
        // Record 0 (VkDrawIndexedIndirectCommand): 3 indices, 1 instance, first index 0, vertex offset 0, instance
        // 0.
        const auto record0  = static_cast<size_t>( GpuInstanceArgsOffset( 0 ) / 4u );
        words[record0 + 0u] = 3u;
        words[record0 + 1u] = 1u;
        if ( const auto wrote = m_ResolveArgs->SetData( words.data(), kResolveArgBytes ); !wrote.IsSuccess() )
        {
            m_ResolveArgs.reset();
            return Common::MakeError(
                 std::format( "VisibilityBuffer: the resolve's arguments: {}", wrote.GetError() ) );
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<VisibilityBufferFrame> VisibilityBuffer::AddFrame( RDG::Builder& graph, const uint32_t width,
                                                                         const uint32_t height )
    {
        if ( !IsInitialized() )
            return Common::MakeError<VisibilityBufferFrame>( "VisibilityBuffer: not initialised" );
        if ( width == 0u || height == 0u )
            return Common::MakeError<VisibilityBufferFrame>( "VisibilityBuffer: the view has no extent" );
        const uint64_t bytes = 16u + 8ull * width * height;
        if ( bytes > UINT32_MAX )
            return Common::MakeError<VisibilityBufferFrame>( std::format(
                 "VisibilityBuffer: {}x{} needs {} bytes, past one storage buffer", width, height, bytes ) );
        if ( !m_Words || m_Width != width || m_Height != height )
        {
            m_Words = ShaderResources::StorageBuffer::Create( "VisibilityBuffer", static_cast<uint32_t>( bytes ),
                                                              62, true );
            if ( !m_Words )
                return Common::MakeError<VisibilityBufferFrame>(
                     std::format( "VisibilityBuffer: {}x{} was not created", width, height ) );
            m_Width  = width;
            m_Height = height;
        }
        Common::BoolResultStr imported = Renderer::ImportBuffer( m_Words, m_WordsImport );
        if ( imported )
            imported = Renderer::ImportBuffer( m_ResolveArgs, m_ResolveArgsImport );
        if ( !imported )
            return Common::MakeError<VisibilityBufferFrame>(
                 std::format( "VisibilityBuffer: not in the frame graph: {}", imported.GetError() ) );

        VisibilityBufferFrame frame;
        frame.Words       = graph.RegisterExternal( m_WordsImport, "VisibilityBuffer" );
        frame.ResolveArgs = graph.RegisterExternal( m_ResolveArgsImport, "VisibilityResolveArgs" );
        frame.Width       = width;
        frame.Height      = height;

        const uint32_t pixels  = width * height;
        const uint32_t groups  = ( pixels + kClearGroupSize - 1u ) / kClearGroupSize;
        const uint32_t groupsX = groups < kClearGroupsRow ? groups : kClearGroupsRow;
        const uint32_t groupsY = ( groups + kClearGroupsRow - 1u ) / kClearGroupsRow;
        graph.AddPass(
             "VirtualGeometry: Visibility Clear", RDG::PassFlags::Compute,
             [this, words = frame.Words]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_ClearLayout.Get( m_Clear->GetSpecification().Shader );
                 pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Clear ) )
                      .Storage( VisibilityClearShaderParameters::Visibility, words, RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
             },
             [this, width, height, groupsX, groupsY]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 const glm::uvec4  extent( width, height, 0u, 0u );
                 bindings.PushConstants( &extent, sizeof( extent ) );
                 return Renderer::DispatchCompute( bindings, *m_Clear, groupsX, groupsY, 1 );
             } );
        return Common::MakeSuccess( frame );
    }

    void VisibilityBuffer::DeclareDepthBindings( RDG::PassBuilder& pass, const RDG::BufferRef words ) const
    {
        if ( !m_Depth || !words.IsValid() )
            return;
        pass.Bindings( m_DepthLayout.Get( m_Depth->GetSpecification().Shader ), RDG::OtherRouteFill{} )
             .Storage( VisibilityDepthShaderParameters::Visibility, words, RDG::Access::StorageRead );
    }

    Common::BoolResultStr VisibilityBuffer::RecordDepth( const RDG::PassContext& context ) const
    {
        if ( !m_Depth )
            return Common::MakeError( "Grass: Visibility Depth recorded without its pipeline" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_Depth, nullptr );
    }
} // namespace Desert::Graphic::VirtualGeometry
