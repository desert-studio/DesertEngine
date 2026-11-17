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

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <vector>

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
        // ... and the ones "Grass: Visibility Material Depth" binds (VisibilityMaterialDepth.shader).
        struct VisibilityMaterialDepthShaderParameters
        {
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
            DESERT_SHADER_PARAMETER( StorageBuffer, Visibility );
            DESERT_SHADER_PARAMETER( StorageBuffer, Sections );
        };
        // ... and the ones "Grass: Visibility Classify" binds (VisibilityClassify.shader): the words read, the
        // tiles' records written after them, and the resolve's record 0 written.
        struct VisibilityClassifyShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Visibility );
            DESERT_SHADER_PARAMETER( StorageBuffer, ResolveArgs );
        };

        // Common/VisibilityTiles.glslh kVisTileSize (VisibilityClassify.shader's LocalSize is one group per tile)
        // and the bytes of one tile's record (four 32-bit words).
        inline constexpr uint32_t kTileSize        = 16u;
        inline constexpr uint64_t kTileRecordBytes = 16u;

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

        // FPS-GRASS-RESOLVE: the tile classification the resolve's draws are culled by (UE Nanite material tiles).
        const auto classifyShader = shaders->GetByName( "VisibilityClassify" );
        if ( !classifyShader )
            return Common::MakeError( "VisibilityBuffer: missing shader 'VisibilityClassify'" );
        const auto classify =
             ComputePipeline::Create( { .Shader = classifyShader, .DebugName = "VisibilityClassify" } );
        if ( !classify )
            return Common::MakeError( std::format( "VisibilityBuffer: {}", classify.GetError() ) );
        m_Classify = classify.GetValue();

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

        // FPS-GRASS-RESOLVE, "Grass: Visibility Material Depth": its own DEPTH32F target (the resolves' cells are
        // built for the G-buffer's layout, whose depth is the same format), every pixel written ALWAYS (a test
        // that always passes: Vulkan writes depth only with the test on), both faces of the full-screen triangle.
        const auto materialShader = shaders->GetByName( "VisibilityMaterialDepth" );
        if ( !materialShader )
            return Common::MakeError( "VisibilityBuffer: missing shader 'VisibilityMaterialDepth'" );
        GraphicsPipelineSpecification material;
        material.DebugName    = "GrassVisibilityMaterialDepth";
        material.Shader       = materialShader;
        material.TargetLayout = RenderTargetLayout{
             .ColorFormats = {}, .DepthFormat = ViewTargetFormats::kGBufferDepth, .Samples = 1 };
        material.DepthTestEnabled  = true;
        material.DepthCompareOp    = CompareOp::Always;
        material.DepthWriteEnabled = true;
        material.CullMode          = CullMode::None;
        const auto materialMade    = GraphicsPipeline::Create( material );
        if ( !materialMade )
            return Common::MakeError(
                 std::format( "VisibilityBuffer: the material depth pass: {}", materialMade.GetError() ) );
        m_MaterialDepth = materialMade.GetValue();

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
        // Record 0 (VkDrawIndexedIndirectCommand): 3 indices, no instance until "Grass: Visibility Classify"
        // writes the frame's two per tile (it runs before every resolve), first index 0, vertex offset 0,
        // instance 0.
        const auto record0  = static_cast<size_t>( GpuInstanceArgsOffset( 0 ) / 4u );
        words[record0 + 0u] = 3u;
        words[record0 + 1u] = 0u;
        if ( const auto wrote = m_ResolveArgs->SetData( words.data(), kResolveArgBytes ); !wrote.IsSuccess() )
        {
            m_ResolveArgs.reset();
            return Common::MakeError(
                 std::format( "VisibilityBuffer: the resolve's arguments: {}", wrote.GetError() ) );
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<VisibilityBufferFrame>
    VisibilityBuffer::AddFrame( RDG::Builder& graph, const uint32_t width, const uint32_t height,
                                const std::span<const uint32_t> sectionFirstRows )
    {
        if ( !IsInitialized() )
            return Common::MakeError<VisibilityBufferFrame>( "VisibilityBuffer: not initialised" );
        if ( width == 0u || height == 0u )
            return Common::MakeError<VisibilityBufferFrame>( "VisibilityBuffer: the view has no extent" );
        // The header, a 64-bit word per pixel, then a record per tile (Common/VisibilityTiles.glslh).
        const uint64_t tiles =
             uint64_t{ ( width + kTileSize - 1u ) / kTileSize } * ( ( height + kTileSize - 1u ) / kTileSize );
        const uint64_t bytes = 16u + 8ull * width * height + kTileRecordBytes * tiles;
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
        // The sections' first rows (VisibilityMaterialDepth.shader's Sections: the count, then the rows), per
        // frame in flight so this frame's upload never lands under the previous frame's reads.
        if ( sectionFirstRows.size() >= UINT32_MAX / 4u )
            return Common::MakeError<VisibilityBufferFrame>( std::format(
                 "VisibilityBuffer: {} sections, past one storage buffer", sectionFirstRows.size() ) );
        const auto sectionCount = static_cast<uint32_t>( sectionFirstRows.size() );
        if ( !m_Sections || sectionCount > m_SectionCapacity )
        {
            const uint32_t capacity = std::max( { sectionCount, m_SectionCapacity * 2u, 64u } );
            m_Sections = ShaderResources::StorageBuffer::Create( "VisibilitySections", 4u * ( capacity + 1u ), 0 );
            if ( !m_Sections )
            {
                m_SectionCapacity = 0;
                return Common::MakeError<VisibilityBufferFrame>(
                     std::format( "VisibilityBuffer: the table of {} sections was not created", capacity ) );
            }
            m_SectionCapacity = capacity;
        }
        std::vector<uint32_t> sections;
        sections.reserve( sectionCount + 1u );
        sections.push_back( sectionCount );
        sections.insert( sections.end(), sectionFirstRows.begin(), sectionFirstRows.end() );
        if ( const auto wrote = m_Sections->SetData( sections.data(), 4u * ( sectionCount + 1u ) );
             !wrote.IsSuccess() )
            return Common::MakeError<VisibilityBufferFrame>(
                 std::format( "VisibilityBuffer: the sections' table: {}", wrote.GetError() ) );

        Common::BoolResultStr imported = Renderer::ImportBuffer( m_Words, m_WordsImport );
        if ( imported )
            imported = Renderer::ImportBuffer( m_ResolveArgs, m_ResolveArgsImport );
        if ( imported )
            imported = Renderer::ImportBuffer( m_Sections, m_SectionsImport );
        if ( !imported )
            return Common::MakeError<VisibilityBufferFrame>(
                 std::format( "VisibilityBuffer: not in the frame graph: {}", imported.GetError() ) );

        VisibilityBufferFrame frame;
        frame.Words       = graph.RegisterExternal( m_WordsImport, "VisibilityBuffer" );
        frame.ResolveArgs = graph.RegisterExternal( m_ResolveArgsImport, "VisibilityResolveArgs" );
        frame.Sections    = graph.RegisterExternal( m_SectionsImport, "VisibilitySections" );
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

    void VisibilityBuffer::AddClassify( RDG::Builder& graph, const VisibilityBufferFrame& frame ) const
    {
        if ( !m_Classify || !frame.IsValid() || frame.Width == 0u || frame.Height == 0u )
            return;
        const uint32_t tilesX = ( frame.Width + kTileSize - 1u ) / kTileSize;
        const uint32_t tilesY = ( frame.Height + kTileSize - 1u ) / kTileSize;
        graph.AddPass(
             "Grass: Visibility Classify", RDG::PassFlags::Compute,
             [this, words = frame.Words, args = frame.ResolveArgs]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_ClassifyLayout.Get( m_Classify->GetSpecification().Shader );
                 pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Classify ) )
                      .Storage( VisibilityClassifyShaderParameters::Visibility, words, RDG::Access::StorageWrite )
                      .Storage( VisibilityClassifyShaderParameters::ResolveArgs, args, RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
             },
             [this, width = frame.Width, height = frame.Height, tilesX,
              tilesY]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 const glm::uvec4  extent( width, height, tilesX, tilesY );
                 bindings.PushConstants( &extent, sizeof( extent ) );
                 return Renderer::DispatchCompute( bindings, *m_Classify, tilesX, tilesY, 1 );
             } );
    }

    void VisibilityBuffer::DeclareDepthBindings( RDG::PassBuilder& pass, const RDG::BufferRef words ) const
    {
        if ( !m_Depth || !words.IsValid() )
            return;
        pass.Bindings( m_DepthLayout.Get( m_Depth->GetSpecification().Shader ), RDG::NoOtherRouteFill() )
             .Storage( VisibilityDepthShaderParameters::Visibility, words, RDG::Access::StorageRead );
    }

    Common::BoolResultStr VisibilityBuffer::RecordDepth( const RDG::PassContext& context ) const
    {
        if ( !m_Depth )
            return Common::MakeError( "Grass: Visibility Depth recorded without its pipeline" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_Depth, nullptr );
    }

    RDG::TextureRef VisibilityBuffer::AddMaterialDepth( RDG::Builder& graph, const VisibilityBufferFrame& frame,
                                                        const RDG::TextureRef sceneDepth ) const
    {
        if ( !m_MaterialDepth || !frame.IsValid() || !frame.Sections.IsValid() || !sceneDepth.IsValid() ||
             frame.Width == 0u || frame.Height == 0u )
            return {};
        RDG::TextureDesc desc;
        desc.Size                           = RDG::Extent3D{ frame.Width, frame.Height, 1u };
        desc.Format                         = ViewTargetFormats::kGBufferDepth;
        const RDG::TextureRef materialDepth = graph.CreateTexture( desc, "GrassMaterialDepth" );
        graph.AddPass(
             "Grass: Visibility Material Depth", RDG::PassFlags::Raster,
             [this, materialDepth, sceneDepth, words = frame.Words,
              sections = frame.Sections]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_MaterialDepthLayout.Get( m_MaterialDepth->GetSpecification().Shader ),
                                RDG::NoOtherRouteFill() )
                      .Sampler( VisibilityMaterialDepthShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( VisibilityMaterialDepthShaderParameters::SceneDepthTexture, sceneDepth,
                                RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                      .Storage( VisibilityMaterialDepthShaderParameters::Visibility, words,
                                RDG::Access::StorageRead )
                      .Storage( VisibilityMaterialDepthShaderParameters::Sections, sections,
                                RDG::Access::StorageRead );
                 // 0: no section's value (DesertVisibilityMaterialDepth is never 0), so a pixel no word won fails
                 // every resolve's EQUAL.
                 pass.DepthTarget( materialDepth, RDG::LoadOp::ClearDepth( 0.0f ), true );
             },
             [this]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DrawFullscreen( bindings, *m_MaterialDepth, nullptr );
             } );
        return materialDepth;
    }
} // namespace Desert::Graphic::VirtualGeometry
