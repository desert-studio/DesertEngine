#include "ComputeImages.hpp"
#include "Shader.hpp"
#include "Pipeline.hpp"
#include "Renderer.hpp"
#include "GpuBatch.hpp"

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/SkyPayload.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <algorithm>

namespace Desert::Graphic
{
    namespace
    {
        std::shared_ptr<Shader> GetComputeShader( const std::string& name )
        {
            return Runtime::ResourceRegistry::GetShaderService()->GetByName( name );
        }

        // Resolves an image handle (panorama 2D or source cube) to its base Image*.
        Image* ResolveInput( const Runtime::ImageHandle& handle )
        {
            return Runtime::ResourceRegistry::GetImageService()->Resolve( handle );
        }
    } // namespace

    std::shared_ptr<Image2D> ComputeImages::ProccessForImage2D( const std::shared_ptr<Image>& /*image*/ )
    {
        return nullptr;
    }

    std::shared_ptr<ImageCube> ComputeImages::ProccessForImageCube( GpuBatch&                         batch,
                                                                    const ComputeImagesSpecification& spec )
    {
        const auto shader = GetComputeShader( spec.ShaderName );
        if ( !shader )
            return nullptr;

        Core::Formats::ImageCubeSpecification outputInfo = {
             .Tag        = spec.Tag,
             .FaceSize   = spec.FaceSize,
             .Format     = Core::Formats::ImageFormat::RGBA32F,
             .Mips       = spec.MipLevels,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        auto output = SP_CAST( ImageCube, ImageCube::Create( outputInfo, nullptr ) );

        Image* input = ResolveInput( spec.InputHandle );
        if ( !input || !output )
            return output;

        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = spec.Tag } );
        if ( !built )
        {
            // The cube comes back allocated and never written, which is what every failure path in this
            // function already hands back — named here so the black environment has a cause in the log.
            LOG_ERROR( "[ComputeImages] '{}' was not convolved: {}", spec.Tag, built.GetError() );
            return output;
        }
        const auto& pipeline = built.GetValue();

        pipeline->SetInput( kComputeImagesInputBinding, input, RDG::Access::SampledCompute,
                            RDG::SubresourceRange::All() );
        pipeline->SetSampler( kComputeImagesSamplerBinding, RDG::SamplerDesc::LinearRepeat() );
        pipeline->SetOutput( kComputeImagesOutputBinding, output.get(), 0 );

        // One thread per face texel (the shaders normalize by `imageSize(outputTexture)`, which is the
        // face). When this dispatch was derived from the old cross-layout Width it launched 4x3 = TWELVE
        // times the invocations the image has texels — for DiffuseIrradiance, where every invocation
        // integrates 65536 samples, 4.8 billion samples per bake instead of 400 million, paid on every
        // 5-degree sun rotation. The spec naming the face is what makes that arithmetic impossible now.
        const uint32_t groups = DispatchGroupCount( spec.FaceSize, kComputeImagesWorkGroupSize );
        pipeline->Record( batch, groups, groups, 6u );
        batch.Retain( pipeline );

        // The compute writes only mip 0; a caller asking for a chain wants the lower levels FILLED, and a
        // requested-but-empty mip is undefined memory behind a valid view (the dispatch's closing barrier
        // makes its write visible to the blits recorded after it in the same batch). Per-face 2D blit mips with
        // clamp addressing are exactly what UE builds for its cubes; seam correctness across faces is the
        // hardware's seamless-cubemap filtering, not ours.
        if ( spec.MipLevels > 1u )
        {
            const auto mipResult =
                 MipMapCubeGenerator::Create( MipGenStrategy::TransferOps )->RecordMips( batch, output );
            if ( !mipResult.IsSuccess() )
                LOG_ERROR( "[ComputeImages] '{}': mip generation failed ({}) — levels 1..{} are undefined.",
                           spec.Tag, mipResult.GetError(), spec.MipLevels - 1u );
        }

        return output;
    }

    std::shared_ptr<ImageCube> ComputeImages::ProccessForImageCubeMips( GpuBatch&                         batch,
                                                                        const ComputeImagesSpecification& spec )
    {
        const auto shader = GetComputeShader( spec.ShaderName );
        if ( !shader )
            return nullptr;

        // The mip chain is bounded by the face (more is an invalid vkCreateImage, VUID-...-00958, which
        // VulkanImageCube now refuses with the numbers). A caller asking for a longer chain than its own
        // face supports is a size/mips disagreement — say so and proceed with the legal chain rather than
        // hand the refusal to every scene as a black environment. (Lit reads the count via
        // textureQueryLevels, so fewer mips is safe — the roughness ramp adapts.)
        const uint32_t faceSize = spec.FaceSize;
        const uint32_t maxMips  = Core::Formats::MipChainLength( faceSize );
        if ( spec.MipLevels > maxMips )
            LOG_ERROR( "[ComputeImages] '{}': {} mips requested for a {}-texel face which supports at most "
                       "{} — building the legal chain. The caller's face/mips pair disagrees.",
                       spec.Tag, spec.MipLevels, faceSize, maxMips );
        const uint32_t mips = std::clamp( spec.MipLevels, 1u, maxMips );

        Core::Formats::ImageCubeSpecification outputInfo = {
             .Tag        = spec.Tag,
             .FaceSize   = spec.FaceSize,
             .Format     = Core::Formats::ImageFormat::RGBA32F,
             .Mips       = mips,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        auto output = SP_CAST( ImageCube, ImageCube::Create( outputInfo, nullptr ) );

        Image* radiance = ResolveInput( spec.InputHandle );
        if ( !radiance || !output )
            return output;

        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = spec.Tag } );
        if ( !built )
        {
            LOG_ERROR( "[ComputeImages] '{}' mip chain was not prefiltered: {}", spec.Tag, built.GetError() );
            return output;
        }
        const auto& pipeline = built.GetValue();

        // One dispatch per mip, each convolved with the matching GGX roughness: the shader derives it from the
        // mip (Common/ReflectionCaptureMip.glslh, the engine's one ramp, which the lighting reads back).
        struct PrefilterPush
        {
            uint32_t Mip      = 0u;
            uint32_t MaxMip   = 0u;
            uint32_t RealTime = 0u; // PrefilterEnvMap's sample count: 0 = UE's reference 1024
        };
        for ( uint32_t mip = 0; mip < mips; ++mip )
        {
            // UE's reference sample count: this bake is a function of the file alone and is cached on disk, so it
            // is paid once (the sky capture, which runs per sky change, takes the real-time count).
            const PrefilterPush push{ mip, mips - 1u, 0u };
            // Dispatch over the FACE size at this mip (the shader writes per-face and clamps to imageSize).
            const uint32_t mipSize   = std::max( 1u, faceSize >> mip );
            const uint32_t groups    = DispatchGroupCount( mipSize, kComputeImagesWorkGroupSize );

            pipeline->SetInput( kComputeImagesInputBinding, radiance, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() );
            pipeline->SetSampler( kComputeImagesSamplerBinding, RDG::SamplerDesc::LinearRepeat() );
            pipeline->SetOutput( kComputeImagesOutputBinding, output.get(), mip );
            pipeline->SetPushConstants( &push, sizeof( push ) );
            pipeline->Record( batch, groups, groups, 6u );
        }
        batch.Retain( pipeline );

        return output;
    }

    std::shared_ptr<ImageCube> ComputeImages::IntegrateIrradianceCube( GpuBatch&                         batch,
                                                                       const ComputeImagesSpecification& spec,
                                                                       uint32_t sourceMip )
    {
        const auto shader = GetComputeShader( spec.ShaderName );
        if ( !shader )
            return nullptr;

        Core::Formats::ImageCubeSpecification outputInfo = {
             .Tag        = spec.Tag,
             .FaceSize   = spec.FaceSize,
             .Format     = Core::Formats::ImageFormat::RGBA32F,
             .Mips       = 1u,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        auto output = SP_CAST( ImageCube, ImageCube::Create( outputInfo, nullptr ) );

        Image* radiance = ResolveInput( spec.InputHandle );
        if ( !radiance || !output )
            return output;

        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = spec.Tag } );
        if ( !built )
        {
            LOG_ERROR( "[ComputeImages] '{}' was not integrated: {}", spec.Tag, built.GetError() );
            return output;
        }
        const auto& pipeline = built.GetValue();

        // The source is ONE mip (the shader sums every texel of what it is given), the output's mip 0.
        // SkyCaptureIrradiance's LocalSize is 8x8, not kComputeImagesWorkGroupSize.
        constexpr uint32_t kIrradianceGroupSize = 8u;
        pipeline->SetInput( kComputeImagesInputBinding, radiance, RDG::Access::SampledCompute,
                            RDG::SubresourceRange::Mip( sourceMip ) );
        pipeline->SetSampler( kComputeImagesSamplerBinding, RDG::SamplerDesc::PointClamp() );
        pipeline->SetOutput( kComputeImagesOutputBinding, output.get(), 0 );
        const uint32_t groups = DispatchGroupCount( spec.FaceSize, kIrradianceGroupSize );
        pipeline->Record( batch, groups, groups, 6u );
        batch.Retain( pipeline );

        return output;
    }

} // namespace Desert::Graphic
