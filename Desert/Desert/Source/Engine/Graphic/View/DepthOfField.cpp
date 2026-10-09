#include "DepthOfField.hpp"

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint32_t kGroupSize = 8; // LocalSize(8, 8, 1) in all six DepthOfField*.shader

        constexpr const char* kKernelShaders[] = { "DepthOfFieldSetup",
                                                   "DepthOfFieldTileFlatten",
                                                   "DepthOfFieldTileDilate",
                                                   "DepthOfFieldGatherForeground",
                                                   "DepthOfFieldGatherBackground",
                                                   "DepthOfFieldRecombine" };
        constexpr const char* kKernelPasses[]  = { "DepthOfField: Setup",
                                                   "DepthOfField: TileFlatten",
                                                   "DepthOfField: TileDilate",
                                                   "DepthOfField: GatherForeground",
                                                   "DepthOfField: GatherBackground",
                                                   "DepthOfField: Recombine" };

        // Half-res colour (rgb) with its signed CoC radius in half-res pixels (a); the gathered layers (rgb, and
        // a = foreground opacity / background presence); the tiles (x foreground radius, y background radius).
        constexpr auto kHalfFormat  = Core::Formats::ImageFormat::RGBA16F;
        constexpr auto kTileFormat  = Core::Formats::ImageFormat::RG16F;
        constexpr auto kLayerFormat = Core::Formats::ImageFormat::RGBA16F;
        constexpr auto kOutFormat   = Core::Formats::ImageFormat::RGBA16F;

        constexpr float kMmPerCm = 10.0f;

        uint32_t GroupCount( uint32_t size )
        {
            return ( size + kGroupSize - 1 ) / kGroupSize;
        }

        std::shared_ptr<const RDG::ShaderBindingLayout> MakeLayout( const char*                  shader,
                                                                    std::vector<RDG::ShaderSlot> slots )
        {
            slots.push_back( { "DepthOfFieldBuffer", RDG::ShaderResourceKind::StorageBuffer } );
            return std::make_shared<const RDG::ShaderBindingLayout>( RDG::ShaderBindingLayout{
                 .ShaderName = shader, .Slots = std::move( slots ), .PushConstantBytes = 0 } );
        }

        // (1 - s / z) / (s - f) in 1/mm: the thin-lens CoC without the A f numerator, signed (z < s negative).
        float ThinLensTerm( const DofLens& lens, float sceneDistanceCm )
        {
            const float s = lens.FocalDistanceCm * kMmPerCm;
            if ( sceneDistanceCm <= 0.0f )
                return -std::numeric_limits<float>::infinity();
            const float z = sceneDistanceCm * kMmPerCm; // +inf stays +inf: s / z = 0, the far limit
            return ( 1.0f - s / z ) / ( s - lens.FocalLengthMm );
        }
    } // namespace

    int DofRingsForQuality( int quality )
    {
        if ( quality <= 0 )
            return 0;
        return quality == 1 ? kDofRingsLow : kDofRingsHigh;
    }

    int DofSampleCount( int rings )
    {
        return rings <= 0 ? 0 : 1 + 4 * rings * ( rings + 1 );
    }

    float DofFocalLengthMm( float sensorWidthMm, const glm::mat4& projection )
    {
        return 0.5f * sensorWidthMm * std::abs( projection[0][0] );
    }

    DofLens MakeDofLens( const DepthOfFieldSettings& settings, const ViewFrame& frame )
    {
        return DofLens{ .FocalDistanceCm = settings.FocalDistanceCm,
                        .FStop           = settings.FStop,
                        .SensorWidthMm   = settings.SensorWidthMm,
                        .FocalLengthMm   = DofFocalLengthMm( settings.SensorWidthMm, frame.Projection ) };
    }

    float DofCocDiameterMm( const DofLens& lens, float sceneDistanceCm )
    {
        const float aperture = lens.FocalLengthMm / lens.FStop; // A = f / N
        return std::abs( aperture * lens.FocalLengthMm * ThinLensTerm( lens, sceneDistanceCm ) );
    }

    float DofCocRadiusPixels( const DofLens& lens, float sceneDistanceCm, uint32_t outputWidth,
                              float maxRadiusPixels )
    {
        const float aperture = lens.FocalLengthMm / lens.FStop;
        const float scale =
             0.5f * aperture * lens.FocalLengthMm * static_cast<float>( outputWidth ) / lens.SensorWidthMm;
        const float radius = scale * ThinLensTerm( lens, sceneDistanceCm );
        return std::clamp( radius, -maxRadiusPixels, maxRadiusPixels );
    }

    float DofMaxRadiusPixels( const DepthOfFieldSettings& settings, ViewExtent output )
    {
        return std::max( settings.MaxBokehPercent, 0.0f ) * 0.01f * static_cast<float>( output.Width );
    }

    glm::vec4 DofDepthToView( const glm::mat4& invProjection )
    {
        return glm::vec4( invProjection[2][2], invProjection[3][2], invProjection[2][3], invProjection[3][3] );
    }

    float DofSceneDistanceCm( float deviceDepth, glm::vec4 depthToView )
    {
        const float w = depthToView.z * deviceDepth + depthToView.w;
        if ( std::abs( w ) < 1e-12f )
            return std::numeric_limits<float>::infinity();
        return std::abs( ( depthToView.x * deviceDepth + depthToView.y ) / w );
    }

    int DofDilateTiles( float maxRadiusOutputPixels )
    {
        const float halfRadius = std::max( maxRadiusOutputPixels, 0.0f ) * 0.5f;
        return static_cast<int>( std::ceil( halfRadius / static_cast<float>( kDofTileSize ) ) );
    }

    bool DepthOfFieldRuns( const DepthOfFieldSettings& settings, const ViewFrame& frame )
    {
        const DofLens lens = MakeDofLens( settings, frame );
        return settings.FocalDistanceCm > 0.0f && settings.FStop > 0.0f && settings.SensorWidthMm > 0.0f &&
               settings.Rings > 0 && settings.MaxBokehPercent > 0.0f && lens.FocalLengthMm > 0.0f &&
               settings.FocalDistanceCm * kMmPerCm > lens.FocalLengthMm && frame.Split.Render.Width > 0 &&
               frame.Split.Render.Height > 0 && frame.Split.Output.Width > 0 && frame.Split.Output.Height > 0;
    }

    ViewExtent DofHalfExtent( ViewExtent output )
    {
        return ViewExtent{ ( output.Width + 1 ) / 2, ( output.Height + 1 ) / 2 };
    }

    RDG::Extent3D DofTileExtent( ViewExtent output )
    {
        const ViewExtent half = DofHalfExtent( output );
        return RDG::Extent3D{ ( half.Width + kDofTileSize - 1 ) / kDofTileSize,
                              ( half.Height + kDofTileSize - 1 ) / kDofTileSize, 1 };
    }

    DofParams MakeDofParams( const DepthOfFieldSettings& settings, const ViewFrame& frame )
    {
        const DofLens       lens  = MakeDofLens( settings, frame );
        const ViewExtent    half  = DofHalfExtent( frame.Split.Output );
        const RDG::Extent3D tiles = DofTileExtent( frame.Split.Output );
        DofParams           params;
        params.OutputSize  = glm::vec2( static_cast<float>( frame.Split.Output.Width ),
                                        static_cast<float>( frame.Split.Output.Height ) );
        params.RenderSize  = glm::vec2( static_cast<float>( frame.Split.Render.Width ),
                                        static_cast<float>( frame.Split.Render.Height ) );
        params.HalfSize    = glm::vec2( static_cast<float>( half.Width ), static_cast<float>( half.Height ) );
        params.TileCount   = glm::vec2( static_cast<float>( tiles.Width ), static_cast<float>( tiles.Height ) );
        params.DepthToView = DofDepthToView( frame.InvProjection );
        params.FocalDistanceMm = lens.FocalDistanceCm * kMmPerCm;
        params.FocalLengthMm   = lens.FocalLengthMm;
        params.CocScale        = lens.FStop > 0.0f && lens.SensorWidthMm > 0.0f
                                      ? 0.5f * ( lens.FocalLengthMm / lens.FStop ) * lens.FocalLengthMm *
                                      params.OutputSize.x / lens.SensorWidthMm
                                      : 0.0f;
        params.MaxRadius       = DofMaxRadiusPixels( settings, frame.Split.Output );
        params.Rings           = settings.Rings;
        params.DilateTiles     = DofDilateTiles( params.MaxRadius );
        return params;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& DofSetupLayout()
    {
        using Kind               = RDG::ShaderResourceKind;
        static const auto layout = MakeLayout( kKernelShaders[0], { { "u_SceneColor", Kind::SampledTexture },
                                                                    { "u_SceneDepth", Kind::SampledTexture },
                                                                    { "u_Half", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& DofTileFlattenLayout()
    {
        using Kind               = RDG::ShaderResourceKind;
        static const auto layout = MakeLayout(
             kKernelShaders[1], { { "u_Half", Kind::SampledTexture }, { "u_Tiles", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& DofTileDilateLayout()
    {
        using Kind               = RDG::ShaderResourceKind;
        static const auto layout = MakeLayout(
             kKernelShaders[2], { { "u_Tiles", Kind::SampledTexture }, { "u_Dilated", Kind::StorageTexture } } );
        return layout;
    }

    namespace
    {
        std::shared_ptr<const RDG::ShaderBindingLayout> MakeGatherLayout( const char* shader )
        {
            using Kind = RDG::ShaderResourceKind;
            return MakeLayout( shader, { { "u_Half", Kind::SampledTexture },
                                         { "u_Dilated", Kind::SampledTexture },
                                         { "u_Layer", Kind::StorageTexture } } );
        }
    } // namespace

    const std::shared_ptr<const RDG::ShaderBindingLayout>& DofGatherForegroundLayout()
    {
        static const auto layout = MakeGatherLayout( kKernelShaders[3] );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& DofGatherBackgroundLayout()
    {
        static const auto layout = MakeGatherLayout( kKernelShaders[4] );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& DofRecombineLayout()
    {
        using Kind               = RDG::ShaderResourceKind;
        static const auto layout = MakeLayout( kKernelShaders[5], { { "u_SceneColor", Kind::SampledTexture },
                                                                    { "u_SceneDepth", Kind::SampledTexture },
                                                                    { "u_Foreground", Kind::SampledTexture },
                                                                    { "u_Background", Kind::SampledTexture },
                                                                    { "u_Output", Kind::StorageTexture } } );
        return layout;
    }

    DepthOfField::DepthOfField()  = default;
    DepthOfField::~DepthOfField() = default;

    Common::ResultStr<RDG::TextureRef> DepthOfField::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                const DepthOfFieldSettings& settings,
                                                                const DepthOfFieldInputs&   inputs ) const
    {
        const auto missing = []( const char* what ) -> Common::ResultStr<RDG::TextureRef>
        {
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", std::format( "DepthOfField: the input {} is not a texture of this graph", what ) );
        };
        if ( !inputs.SceneColor.IsValid() )
            return missing( "SceneColor" );
        if ( !inputs.SceneDepth.IsValid() )
            return missing( "SceneDepth" );
        if ( !DepthOfFieldRuns( settings, frame ) )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}",
                 std::format( "DepthOfField: nothing to defocus (focus {} cm, f/{}, sensor {} mm, focal "
                              "length {} mm, max bokeh {} %, {} rings, split {}x{} -> {}x{})",
                              settings.FocalDistanceCm, settings.FStop, settings.SensorWidthMm,
                              DofFocalLengthMm( settings.SensorWidthMm, frame.Projection ),
                              settings.MaxBokehPercent, settings.Rings, frame.Split.Render.Width,
                              frame.Split.Render.Height, frame.Split.Output.Width, frame.Split.Output.Height ) );

        const ViewExtent    halfExtent = DofHalfExtent( frame.Split.Output );
        const RDG::Extent3D half{ halfExtent.Width, halfExtent.Height, 1 };
        const RDG::Extent3D output{ frame.Split.Output.Width, frame.Split.Output.Height, 1 };
        const RDG::Extent3D tileExtent = DofTileExtent( frame.Split.Output );

        RDG::TextureDesc desc;
        desc.Size                        = half;
        desc.Format                      = kHalfFormat;
        const RDG::TextureRef halfColor  = graph.CreateTexture( desc, "DepthOfField.Half" );
        desc.Format                      = kLayerFormat;
        const RDG::TextureRef foreground = graph.CreateTexture( desc, "DepthOfField.Foreground" );
        const RDG::TextureRef background = graph.CreateTexture( desc, "DepthOfField.Background" );
        desc.Size                        = tileExtent;
        desc.Format                      = kTileFormat;
        const RDG::TextureRef tiles      = graph.CreateTexture( desc, "DepthOfField.Tiles" );
        const RDG::TextureRef dilated    = graph.CreateTexture( desc, "DepthOfField.DilatedTiles" );
        desc.Size                        = output;
        desc.Format                      = kOutFormat;
        const RDG::TextureRef result     = graph.CreateTexture( desc, "DepthOfField.Output" );

        const DofParams      params = MakeDofParams( settings, frame );
        const RDG::BufferRef buffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( DofParams ) }, "DepthOfField.Params" );
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
                return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, GroupCount( extent.Width ),
                                                                GroupCount( extent.Height ), 1 );
            };
        };
        const auto point  = RDG::SamplerDesc::PointClamp();
        const auto linear = RDG::SamplerDesc::LinearClamp();
        const auto all    = RDG::SubresourceRange::All();

        // 1. Setup: the 2x2 average (one bilinear tap at the half-res pixel's centre) and the CoC of the depth.
        graph.AddPass(
             kKernelPasses[0], RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( DofSetupLayout(), RDG::OtherRouteFill{} )
                      .Sampled( "u_SceneColor", inputs.SceneColor, RDG::Access::SampledCompute, all, linear )
                      .Sampled( "u_SceneDepth", inputs.SceneDepth, RDG::Access::SampledCompute, all, point )
                      .Storage( "u_Half", halfColor, RDG::Access::StorageWrite, 0 )
                      .Storage( "DepthOfFieldBuffer", buffer, RDG::Access::StorageRead );
             },
             exec( Kernel::Setup, half ) );

        graph.AddPass(
             kKernelPasses[1], RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( DofTileFlattenLayout(), RDG::OtherRouteFill{} )
                      .Sampled( "u_Half", halfColor, RDG::Access::SampledCompute, all, point )
                      .Storage( "u_Tiles", tiles, RDG::Access::StorageWrite, 0 )
                      .Storage( "DepthOfFieldBuffer", buffer, RDG::Access::StorageRead );
             },
             exec( Kernel::TileFlatten, tileExtent ) );

        graph.AddPass(
             kKernelPasses[2], RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( DofTileDilateLayout(), RDG::OtherRouteFill{} )
                      .Sampled( "u_Tiles", tiles, RDG::Access::SampledCompute, all, point )
                      .Storage( "u_Dilated", dilated, RDG::Access::StorageWrite, 0 )
                      .Storage( "DepthOfFieldBuffer", buffer, RDG::Access::StorageRead );
             },
             exec( Kernel::TileDilate, tileExtent ) );

        // 4/5. The two layers are gathered apart (UE: foreground and background never share a kernel).
        const auto gather = [&]( Kernel kernel, const std::shared_ptr<const RDG::ShaderBindingLayout>& layout,
                                 RDG::TextureRef layer )
        {
            graph.AddPass(
                 kKernelPasses[static_cast<std::size_t>( kernel )], RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( layout, RDG::OtherRouteFill{} )
                          .Sampled( "u_Half", halfColor, RDG::Access::SampledCompute, all, linear )
                          .Sampled( "u_Dilated", dilated, RDG::Access::SampledCompute, all, point )
                          .Storage( "u_Layer", layer, RDG::Access::StorageWrite, 0 )
                          .Storage( "DepthOfFieldBuffer", buffer, RDG::Access::StorageRead );
                 },
                 exec( kernel, half ) );
        };
        gather( Kernel::GatherForeground, DofGatherForegroundLayout(), foreground );
        gather( Kernel::GatherBackground, DofGatherBackgroundLayout(), background );

        graph.AddPass(
             kKernelPasses[5], RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( DofRecombineLayout(), RDG::OtherRouteFill{} )
                      .Sampled( "u_SceneColor", inputs.SceneColor, RDG::Access::SampledCompute, all, point )
                      .Sampled( "u_SceneDepth", inputs.SceneDepth, RDG::Access::SampledCompute, all, point )
                      .Sampled( "u_Foreground", foreground, RDG::Access::SampledCompute, all, linear )
                      .Sampled( "u_Background", background, RDG::Access::SampledCompute, all, linear )
                      .Storage( "u_Output", result, RDG::Access::StorageWrite, 0 )
                      .Storage( "DepthOfFieldBuffer", buffer, RDG::Access::StorageRead );
             },
             exec( Kernel::Recombine, output ) );

        return Common::MakeSuccess( result );
    }

    std::shared_ptr<ComputePipeline> DepthOfField::PipelineFor( Kernel kernel, std::string& error ) const
    {
        const auto index = static_cast<std::size_t>( kernel );
        if ( index >= m_Pipelines.size() )
        {
            error = std::format( "DepthOfField: kernel {} has no shader", index );
            return nullptr;
        }

        const std::lock_guard lock( m_PipelineMutex );
        if ( m_Pipelines[index] )
            return m_Pipelines[index];

        const char*             name    = kKernelShaders[index];
        Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
        if ( !service )
        {
            error = std::format( "DepthOfField: no shader service to compile '{}'", name );
            return nullptr;
        }
        const std::shared_ptr<Shader> shader = service->AcquireVariant( name, Core::ShaderVariant{} );
        if ( !shader )
        {
            error = std::format( "DepthOfField: the shader '{}' did not compile — expected "
                                 "Editor/Resources/Shaders/Programs/DepthOfField/{}.shader",
                                 name, name );
            return nullptr;
        }
        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
        if ( !built )
        {
            error = std::format( "DepthOfField: pipeline '{}': {}", name, built.GetError() );
            return nullptr;
        }
        m_Pipelines[index] = built.GetValue();
        return m_Pipelines[index];
    }
} // namespace Desert::Graphic
