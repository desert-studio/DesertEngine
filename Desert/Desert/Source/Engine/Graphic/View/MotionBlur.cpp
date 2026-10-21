#include "MotionBlur.hpp"

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <algorithm>
#include <format>
#include <string>

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint32_t kBlurGroupSize = 8; // LocalSize(8, 8, 1) in all four MotionBlur*.shader

        constexpr const char* kBlurKernelShaders[] = { "MotionBlurFlatten", "MotionBlurTileMax",
                                                       "MotionBlurNeighborMax", "MotionBlurGather" };
        constexpr const char* kBlurKernelPasses[]  = { "MotionBlur: Flatten", "MotionBlur: TileMax",
                                                       "MotionBlur: NeighborMax", "MotionBlur: Gather" };

        // Flattened velocity: output-pixel velocity (xy) and the scene's device depth (z).
        constexpr auto kFlatFormat     = Core::Formats::ImageFormat::RGBA16F;
        constexpr auto kBlurTileFormat = Core::Formats::ImageFormat::RG16F;
        constexpr auto kBlurOutFormat  = Core::Formats::ImageFormat::RGBA16F;

        uint32_t BlurGroupCount( uint32_t size )
        {
            return ( size + kBlurGroupSize - 1 ) / kBlurGroupSize;
        }

        std::shared_ptr<const RDG::ShaderBindingLayout> MakeBlurLayout( const char*                  shader,
                                                                        std::vector<RDG::ShaderSlot> slots )
        {
            slots.push_back( { "MotionBlurBuffer", RDG::ShaderResourceKind::StorageBuffer } );
            return std::make_shared<const RDG::ShaderBindingLayout>( RDG::ShaderBindingLayout{
                 .ShaderName = shader, .Slots = std::move( slots ), .PushConstantBytes = 0 } );
        }
    } // namespace

    int MotionBlurSamplesForQuality( int quality )
    {
        if ( quality <= 0 )
            return 0;
        return quality == 1 ? kMotionBlurSamplesLow : kMotionBlurSamplesHigh;
    }

    float MotionBlurVelocityScale( const MotionBlurSettings& settings, float deltaSeconds )
    {
        if ( settings.TargetFPS <= 0.0f )
            return settings.Amount;
        if ( deltaSeconds <= 0.0f )
            return 0.0f;
        return settings.Amount / ( settings.TargetFPS * deltaSeconds );
    }

    bool MotionBlurRuns( const MotionBlurSettings& settings, const ViewFrame& frame )
    {
        return settings.Amount > 0.0f && settings.Samples > 0 && settings.MaxPercent > 0.0f &&
               MotionBlurVelocityScale( settings, frame.DeltaSeconds ) > 0.0f && frame.Split.Render.Width > 0 &&
               frame.Split.Render.Height > 0 && frame.Split.Output.Width > 0 && frame.Split.Output.Height > 0;
    }

    float MotionBlurMaxPixels( const MotionBlurSettings& settings, ViewExtent output )
    {
        return std::max( settings.MaxPercent, 0.0f ) * 0.01f * static_cast<float>( output.Width );
    }

    glm::vec2 MotionBlurPixelVelocity( glm::vec2 velocityNdc, float scale, ViewExtent output, float maxPixels )
    {
        const glm::vec2 pixels =
             velocityNdc * glm::vec2( 0.5f, -0.5f ) *
             glm::vec2( static_cast<float>( output.Width ), static_cast<float>( output.Height ) ) * scale;
        const float length = glm::length( pixels );
        return length > maxPixels && length > 0.0f ? pixels * ( maxPixels / length ) : pixels;
    }

    glm::vec2 LongestVelocity( std::span<const glm::vec2> velocities )
    {
        glm::vec2 longest( 0.0f );
        float     longestSq = 0.0f;
        for ( const glm::vec2& v : velocities )
        {
            const float lengthSq = glm::dot( v, v );
            if ( lengthSq > longestSq )
            {
                longest   = v;
                longestSq = lengthSq;
            }
        }
        return longest;
    }

    glm::vec2 NeighborhoodMaxVelocity( std::span<const glm::vec2> tiles, uint32_t width, uint32_t height,
                                       uint32_t x, uint32_t y )
    {
        std::array<glm::vec2, 9> neighbourhood{};
        std::size_t              count = 0;
        for ( int dy = -1; dy <= 1; ++dy )
            for ( int dx = -1; dx <= 1; ++dx )
            {
                const auto tx = static_cast<uint32_t>(
                     std::clamp( static_cast<int>( x ) + dx, 0, static_cast<int>( width ) - 1 ) );
                const auto ty = static_cast<uint32_t>(
                     std::clamp( static_cast<int>( y ) + dy, 0, static_cast<int>( height ) - 1 ) );
                const std::size_t index = std::size_t{ ty } * width + tx;
                if ( index < tiles.size() )
                    neighbourhood[count++] = tiles[index];
            }
        return LongestVelocity( std::span<const glm::vec2>( neighbourhood.data(), count ) );
    }

    RDG::Extent3D MotionBlurTileExtent( ViewExtent render )
    {
        return RDG::Extent3D{ ( render.Width + kMotionBlurTileSize - 1 ) / kMotionBlurTileSize,
                              ( render.Height + kMotionBlurTileSize - 1 ) / kMotionBlurTileSize, 1 };
    }

    MotionBlurParams MakeMotionBlurParams( const MotionBlurSettings& settings, const ViewFrame& frame )
    {
        const RDG::Extent3D tiles = MotionBlurTileExtent( frame.Split.Render );
        MotionBlurParams    params;
        params.RenderSize    = glm::vec2( static_cast<float>( frame.Split.Render.Width ),
                                          static_cast<float>( frame.Split.Render.Height ) );
        params.OutputSize    = glm::vec2( static_cast<float>( frame.Split.Output.Width ),
                                          static_cast<float>( frame.Split.Output.Height ) );
        params.TileCount     = glm::vec2( static_cast<float>( tiles.Width ), static_cast<float>( tiles.Height ) );
        params.VelocityScale = MotionBlurVelocityScale( settings, frame.DeltaSeconds );
        params.MaxPixels     = MotionBlurMaxPixels( settings, frame.Split.Output );
        params.Samples       = settings.Samples;
        return params;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& MotionBlurFlattenLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const auto layout =
             MakeBlurLayout( kBlurKernelShaders[0], { { "VelocityTexture", Kind::Texture },
                                                      { "SceneDepthTexture", Kind::Texture },
                                                      { "PointClampSampler", Kind::Sampler },
                                                      { "FlattenedVelocityTexture", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& MotionBlurTileMaxLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const auto layout =
             MakeBlurLayout( kBlurKernelShaders[1], { { "FlattenedVelocityTexture", Kind::Texture },
                                                      { "PointClampSampler", Kind::Sampler },
                                                      { "TileMaxTexture", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& MotionBlurNeighborMaxLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const auto layout =
             MakeBlurLayout( kBlurKernelShaders[2], { { "TileMaxTexture", Kind::Texture },
                                                      { "PointClampSampler", Kind::Sampler },
                                                      { "NeighborMaxTexture", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& MotionBlurGatherLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const auto layout =
             MakeBlurLayout( kBlurKernelShaders[3], { { "SceneColorTexture", Kind::Texture },
                                                      { "FlattenedVelocityTexture", Kind::Texture },
                                                      { "NeighborMaxTexture", Kind::Texture },
                                                      { "LinearClampSampler", Kind::Sampler },
                                                      { "PointClampSampler", Kind::Sampler },
                                                      { "BlurredSceneColorTexture", Kind::StorageTexture } } );
        return layout;
    }

    MotionBlur::MotionBlur()  = default;
    MotionBlur::~MotionBlur() = default;

    Common::ResultStr<RDG::TextureRef> MotionBlur::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                              const MotionBlurSettings& settings,
                                                              const MotionBlurInputs&   inputs ) const
    {
        const auto missing = []( const char* what ) -> Common::ResultStr<RDG::TextureRef>
        {
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", std::format( "MotionBlur: the input {} is not a texture of this graph", what ) );
        };
        if ( !inputs.SceneColor.IsValid() )
            return missing( "SceneColor" );
        if ( !inputs.SceneDepth.IsValid() )
            return missing( "SceneDepth" );
        if ( !inputs.Velocity.IsValid() )
            return missing( "Velocity" );
        if ( !MotionBlurRuns( settings, frame ) )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", std::format( "MotionBlur: nothing to blur (amount {}, max {} %, target {} fps, {} samples, "
                                    "delta {} s, split {}x{} -> {}x{})",
                                    settings.Amount, settings.MaxPercent, settings.TargetFPS, settings.Samples,
                                    frame.DeltaSeconds, frame.Split.Render.Width, frame.Split.Render.Height,
                                    frame.Split.Output.Width, frame.Split.Output.Height ) );

        const RDG::Extent3D render{ frame.Split.Render.Width, frame.Split.Render.Height, 1 };
        const RDG::Extent3D output{ frame.Split.Output.Width, frame.Split.Output.Height, 1 };
        const RDG::Extent3D tileExtent = MotionBlurTileExtent( frame.Split.Render );

        RDG::TextureDesc desc;
        desc.Size                         = render;
        desc.Format                       = kFlatFormat;
        const RDG::TextureRef flat        = graph.CreateTexture( desc, "MotionBlur.Flat" );
        desc.Size                         = tileExtent;
        desc.Format                       = kBlurTileFormat;
        const RDG::TextureRef tileMax     = graph.CreateTexture( desc, "MotionBlur.TileMax" );
        const RDG::TextureRef neighborMax = graph.CreateTexture( desc, "MotionBlur.NeighborMax" );
        desc.Size                         = output;
        desc.Format                       = kBlurOutFormat;
        const RDG::TextureRef blurred     = graph.CreateTexture( desc, "MotionBlur.Output" );

        const MotionBlurParams params = MakeMotionBlurParams( settings, frame );
        const RDG::BufferRef   buffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( MotionBlurParams ) }, "MotionBlur.Params" );
        graph.QueueBufferUpload( buffer, std::as_bytes( std::span( &params, 1 ) ) );

        const auto exec = [this]( Kernel kernel, RDG::Extent3D extent )
        {
            return [this, kernel, extent]( RDG::PassContext& context ) -> Common::BoolResultStr
            {
                std::string                            error;
                const std::shared_ptr<ComputePipeline> pipeline = PipelineFor( kernel, error );
                if ( !pipeline )
                    return Common::MakeError( error );
                const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                return Renderer::GetInstance().DispatchCompute(
                     bindings, *pipeline, BlurGroupCount( extent.Width ), BlurGroupCount( extent.Height ), 1 );
            };
        };
        const auto point = RDG::SamplerDesc::PointClamp();

        graph.AddPass(
             kBlurKernelPasses[0], RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( MotionBlurFlattenLayout(), RDG::OtherRouteFill{} )
                      .Texture( "VelocityTexture", inputs.Velocity, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Sampler( "PointClampSampler", point )
                      .Texture( "SceneDepthTexture", inputs.SceneDepth, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Sampler( "PointClampSampler", point )
                      .Storage( "FlattenedVelocityTexture", flat, RDG::Access::StorageWrite, 0 )
                      .Storage( "MotionBlurBuffer", buffer, RDG::Access::StorageRead );
             },
             exec( Kernel::Flatten, render ) );

        graph.AddPass(
             kBlurKernelPasses[1], RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( MotionBlurTileMaxLayout(), RDG::OtherRouteFill{} )
                      .Texture( "FlattenedVelocityTexture", flat, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( "PointClampSampler", point )
                      .Storage( "TileMaxTexture", tileMax, RDG::Access::StorageWrite, 0 )
                      .Storage( "MotionBlurBuffer", buffer, RDG::Access::StorageRead );
             },
             exec( Kernel::TileMax, tileExtent ) );

        graph.AddPass(
             kBlurKernelPasses[2], RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( MotionBlurNeighborMaxLayout(), RDG::OtherRouteFill{} )
                      .Texture( "TileMaxTexture", tileMax, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( "PointClampSampler", point )
                      .Storage( "NeighborMaxTexture", neighborMax, RDG::Access::StorageWrite, 0 )
                      .Storage( "MotionBlurBuffer", buffer, RDG::Access::StorageRead );
             },
             exec( Kernel::NeighborMax, tileExtent ) );

        graph.AddPass(
             kBlurKernelPasses[3], RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 // The colour taps are bilinear (sub-pixel positions along the velocity); the flattened velocity
                 // and the tiles are read at texel centres.
                 pass.Bindings( MotionBlurGatherLayout(), RDG::OtherRouteFill{} )
                      .Texture( "SceneColorTexture", inputs.SceneColor, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Sampler( "LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                      .Texture( "FlattenedVelocityTexture", flat, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( "PointClampSampler", point )
                      .Texture( "NeighborMaxTexture", neighborMax, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Sampler( "PointClampSampler", point )
                      .Storage( "BlurredSceneColorTexture", blurred, RDG::Access::StorageWrite, 0 )
                      .Storage( "MotionBlurBuffer", buffer, RDG::Access::StorageRead );
             },
             exec( Kernel::Gather, output ) );

        return Common::MakeSuccess( blurred );
    }

    std::shared_ptr<ComputePipeline> MotionBlur::PipelineFor( Kernel kernel, std::string& error ) const
    {
        const auto index = static_cast<std::size_t>( kernel );
        if ( index >= m_Pipelines.size() )
        {
            error = std::format( "MotionBlur: kernel {} has no shader", index );
            return nullptr;
        }

        const std::lock_guard lock( m_PipelineMutex );
        if ( m_Pipelines[index] )
            return m_Pipelines[index];

        const char*             name    = kBlurKernelShaders[index];
        Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
        if ( !service )
        {
            error = std::format( "MotionBlur: no shader service to compile '{}'", name );
            return nullptr;
        }
        const std::shared_ptr<Shader> shader = service->GetByName( name );
        if ( !shader )
        {
            error = std::format( "MotionBlur: the shader '{}' did not compile — expected "
                                 "Editor/Resources/Shaders/Programs/MotionBlur/{}.shader",
                                 name, name );
            return nullptr;
        }
        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
        if ( !built )
        {
            error = std::format( "MotionBlur: pipeline '{}': {}", name, built.GetError() );
            return nullptr;
        }
        m_Pipelines[index] = built.GetValue();
        return m_Pipelines[index];
    }
} // namespace Desert::Graphic
