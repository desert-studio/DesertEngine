#include "ComputeImages.hpp"
#include "Shader.hpp"
#include "Pipeline.hpp"
#include "Renderer.hpp"

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Image.hpp>
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

    std::shared_ptr<Image2D> ComputeImages::BakeProceduralPanorama( uint32_t width, uint32_t height,
                                                                    ShaderResources::StorageBuffer* skyParams,
                                                                    Image2D*                transmittanceLut,
                                                                    Image2D*                multiScatterLut,
                                                                    const CloudBakeBinding& clouds )
    {
        // THE FOURTH CONSUMER OF THE CLOUD MEDIUM, and the reason the medium is a compile-time include
        // rather than a generated program: this is the SKY's program — atmosphere and clouds in one
        // panorama, needed by scenes with no cloud layer — so it can never become a pass of a cloud
        // material, and yet the light it puts into the world has to come from the same clouds the camera
        // sees. Compiling the same file under the layer's variant is what makes those two the same
        // clouds; a second analytic dome beside them is the shape that produced the grey-clouds defect.
        //
        // The variant program is held only for the duration of this bake, which is what its lifetime
        // should be: the bake is a submit-and-wait, and nothing after it needs the modules.
        std::shared_ptr<Shader> shader;
        if ( clouds.Medium && !clouds.Medium->IsDefault() )
        {
            shader = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( "BakeProceduralSky",
                                                                                    *clouds.Medium );
            if ( !shader )
            {
                // Named, then the shipped program: an authored medium that will not compile must not take
                // the scene's lighting away with it.
                LOG_ERROR( "[Sky] the authored cloud medium could not be compiled into BakeProceduralSky "
                           "— the panorama that lights this scene falls back to the shipped medium, so "
                           "the world is lit by a different sky than the one on screen." );
            }
        }
        if ( !shader )
            shader = GetComputeShader( "BakeProceduralSky" );
        if ( !shader || !skyParams )
            return nullptr;

        Core::Formats::Image2DSpecification outputInfo = {
             .Tag        = "ProceduralSkyPanorama",
             .Width      = width,
             .Height     = height,
             .Format     = Core::Formats::ImageFormat::RGBA32F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        auto output = Image2D::Create( outputInfo );
        if ( !output )
            return nullptr;

        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = "BakeProceduralSky" } );
        if ( !built )
        {
            // The panorama that lights the scene is not baked, and the caller reads a null image as
            // "no environment" — the same outcome the shader-missing branch above produces, said with
            // the reason rather than by falling over.
            LOG_ERROR( "[ComputeImages] the procedural sky panorama was not baked: {}", built.GetError() );
            return nullptr;
        }
        const auto& pipeline = built.GetValue();

        pipeline->SetOutput( 0, output.get(), 0 );
        // The bake reads the sky through the same std430 block the screen pass does — no second hand-packed
        // mirror to keep in step. The binding is passed EXPLICITLY here while the graphics path uses the
        // buffer's own binding number, so the two must be the same constant or the descriptor aliases
        // something else entirely (that failure is a validation error, not a wrong picture).
        pipeline->SetStorageBuffer( kSkyPayloadBinding, skyParams );

        // The physical model's march reads the cached atmosphere LUTs; the gradient model never
        // samples these bindings, but a compute binding nobody writes gets NO descriptor write at all
        // (VulkanPipelineCompute only writes m_BoundInputs) — bind the fallbacks so both branches of
        // the shader are always backed by SOMETHING valid.
        //
        // RGBA32F because the fallback TABLE only holds RGBA8F and RGBA32F (VulkanFallbackTextures.cpp),
        // and RGBA32F is what VulkanMaterialBackend already substitutes for a declared-but-unbound
        // Sampled2D — the identical situation. Asking for the LUTs' own RGBA16F threw std::out_of_range
        // out of that table and killed every gradient scene at the bake. The format is irrelevant here
        // anyway: a sampled descriptor only needs a valid 2D view, and the branch that would read it is
        // not taken.
        auto& fallbacks = FallbackTextures::Get();
        pipeline->SetInput( kSkyTransmittanceLutBinding,
                            transmittanceLut
                                 ? transmittanceLut
                                 : fallbacks.GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA32F ).get() );
        pipeline->SetInput( kSkyMultiScatterLutBinding,
                            multiScatterLut
                                 ? multiScatterLut
                                 : fallbacks.GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA32F ).get() );

        // ---- THE CLOUD LAYER -------------------------------------------------------------------------
        //
        // EVERY ONE OF THESE IS WRITTEN ON EVERY PATH, whether the layer exists or not. A declared
        // descriptor that nobody writes makes the set INVALID, and VulkanPipelineCompute answers an
        // invalid set by returning without dispatching — so a scene with no clouds would come back with an
        // uninitialised panorama and a black environment, and nothing anywhere would say why. The gate is
        // the push constant below and nothing else.
        //
        // The parameter buffers are the SKY renderer's: the cloud renderer's own are per-frame resources
        // written by the passes inside the frame, and this bake is issued before them.
        auto* volumeFallback = fallbacks.GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F ).get();

        // THE TWO BLOCKS ARE BOUND WHETHER OR NOT THERE ARE CLOUDS, and that is the whole content of this
        // branch. A cloudless scene — two of the repository's fifty-one, plus every asset thumbnail and
        // every mesh preview — still has these two descriptors declared by the shader, so leaving them
        // unwritten would invalidate the set and cost that scene its ENTIRE environment, not its clouds.
        // The caller creates both at initialization and uploads to them on every path, so a null here is a
        // failed allocation and not a state: it is the one case that has to be said out loud.
        if ( clouds.Params && clouds.Authored )
        {
            pipeline->SetStorageBuffer( kSkyBakeCloudParamsBinding, clouds.Params );
            pipeline->SetStorageBuffer( kSkyBakeCloudAuthoredBinding, clouds.Authored );
        }
        else
        {
            LOG_ERROR( "[SkyAtmosphere] The environment bake has no cloud parameter buffer to bind "
                       "(params={}, authored={}). Both are created with the sky's own, so this is an "
                       "allocation failure — the dispatch below will be skipped for an invalid descriptor "
                       "set and the environment will come back black.",
                       clouds.Params != nullptr, clouds.Authored != nullptr );
        }

        const bool cloudsBound = clouds.Marched && clouds.Params != nullptr && clouds.Authored != nullptr;

        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
        {
            Image3D* noise = cloudsBound ? clouds.Noise[slot] : nullptr;
            pipeline->SetInput( kSkyBakeCloudNoiseBindings[slot], noise ? noise : volumeFallback );
        }

        pipeline->SetInput( kSkyBakeCloudModellingBinding,
                            cloudsBound && clouds.Modelling ? clouds.Modelling : volumeFallback );
        pipeline->SetInput( kSkyBakeCloudAuthoredAtlasBinding,
                            cloudsBound && clouds.AuthoredAtlas ? clouds.AuthoredAtlas : volumeFallback );
        pipeline->SetInput( kSkyBakeCloudSkyOcclusionBinding,
                            cloudsBound && clouds.SkyOcclusion && clouds.SkyOcclusionVolume
                                 ? clouds.SkyOcclusionVolume
                                 : volumeFallback );
        pipeline->SetInput( kSkyBakeDistantSkyLightBinding,
                            clouds.DistantSkyLight
                                 ? clouds.DistantSkyLight
                                 : fallbacks.GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA32F ).get() );

        // ---- THE AUTHORED MEDIUM'S OWN RESOURCES -----------------------------------------------------
        //
        // THE FOURTH BINDING SITE OF THE SAME TWO THINGS, and the one that is easiest to forget because it
        // is in another renderer's file. A medium that reached the march, the shadow map and the occlusion
        // volume but not this pass would light the world from a sky judged by a different tint than the one
        // on screen — the grey-clouds shape, arriving through the IBL.
        //
        // BOUND ONLY WHEN THE MEDIUM DECLARES THEM, and "declares" is not a guess: the emitter writes a
        // block only for properties its five functions read and a sampler only per Texture2D property, so
        // a non-empty MediumValues/MediumImages here means the program above has exactly those slots. The
        // gate is not `Marched`: this is the medium's own layout, which exists whether or not the layer is
        // marching this frame.
        if ( clouds.MediumParams )
            pipeline->SetStorageBuffer( Core::kCloudMediumParamsBinding, clouds.MediumParams );

        // EVERY DECLARED SLOT IS WRITTEN, fallback included, for the reason the whole cloud block above is:
        // a declared sampler left unwritten invalidates the set and this pass answers that by not
        // dispatching, which costs the scene its entire environment rather than its clouds.
        for ( std::size_t slot = 0; slot < clouds.MediumImages.size(); ++slot )
            pipeline->SetInput(
                 Core::kCloudMediumTextureFirst + static_cast<uint32_t>( slot ),
                 clouds.MediumImages[slot]
                      ? clouds.MediumImages[slot]
                      : fallbacks.GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA32F ).get() );

        // x marches, y reads the sky-occlusion volume, z is the aerial perspective's start depth, w applies
        // the atmosphere's transmittance at each cloud sample's own altitude. The last three are ANDed with
        // the buffers above for the reason the march's own gates are: a layer whose flag is on but whose
        // resource was not written must fall back rather than read an image nobody filled.
        //
        // w IS ALSO ANDed WITH transmittanceLut, and that is not belt and braces. The cloud renderer raised
        // it because the SKY published a LUT; this function is the last place that can see whether the LUT
        // it is BINDING is that one or the fallback texture, and a fallback read as an atmosphere is a
        // transmittance of whatever colour the fallback happens to be.
        const bool perSampleSun = cloudsBound && clouds.PerSampleSunTransmittance && transmittanceLut != nullptr;

        const glm::vec4 cloudPush{ cloudsBound ? 1.0f : 0.0f,
                                   cloudsBound && clouds.SkyOcclusion && clouds.SkyOcclusionVolume ? 1.0f : 0.0f,
                                   std::max( clouds.AerialStartDepthKm, 0.0f ), perSampleSun ? 1.0f : 0.0f };
        pipeline->SetPushConstants( &cloudPush, static_cast<uint32_t>( sizeof( cloudPush ) ) );

        pipeline->Dispatch( std::max( 1u, width / kComputeImagesWorkGroupSize ),
                            std::max( 1u, height / kComputeImagesWorkGroupSize ), 1u );

        return output;
    }

    std::shared_ptr<ImageCube> ComputeImages::ProccessForImageCube( const ComputeImagesSpecification& spec )
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

        pipeline->SetInput( 0, input );
        pipeline->SetOutput( 1, output.get(), 0 );

        // The authored sky, pushed UNCONDITIONALLY. Both programs this function ever runs declare the
        // block, and a declared-but-unwritten push-constant range is undefined memory — so "only push it
        // when the look is not identity" would make the procedural sky's bake read whatever the previous
        // dispatch left behind. Identity is a value, not an absence.
        const SkyLookPush push = MakeSkyLookPush( spec.Look );
        pipeline->SetPushConstants( &push, sizeof( push ) );

        // One thread per face texel (the shaders normalize by `imageSize(outputTexture)`, which is the
        // face). When this dispatch was derived from the old cross-layout Width it launched 4x3 = TWELVE
        // times the invocations the image has texels — for DiffuseIrradiance, where every invocation
        // integrates 65536 samples, 4.8 billion samples per bake instead of 400 million, paid on every
        // 5-degree sun rotation. The spec naming the face is what makes that arithmetic impossible now.
        const uint32_t groups = DispatchGroupCount( spec.FaceSize, kComputeImagesWorkGroupSize );
        pipeline->Dispatch( groups, groups, 6u );

        // The compute writes only mip 0; a caller asking for a chain wants the lower levels FILLED, and a
        // requested-but-empty mip is undefined memory behind a valid view (Dispatch above is the immediate
        // fenced path, so the blits ordering after it is safe). Per-face 2D blit mips with clamp
        // addressing are exactly what UE builds for its cubes; seam correctness across faces is the
        // hardware's seamless-cubemap filtering, not ours.
        if ( spec.MipLevels > 1u )
        {
            const auto mipResult =
                 MipMapCubeGenerator::Create( MipGenStrategy::TransferOps )->GenerateMips( output );
            if ( !mipResult.IsSuccess() )
                LOG_ERROR( "[ComputeImages] '{}': mip generation failed ({}) — levels 1..{} are undefined.",
                           spec.Tag, mipResult.GetError(), spec.MipLevels - 1u );
        }

        return output;
    }

    std::shared_ptr<ImageCube> ComputeImages::ProccessForImageCubeMips( const ComputeImagesSpecification& spec )
    {
        const auto shader = GetComputeShader( spec.ShaderName );
        if ( !shader )
            return nullptr;

        // The mip chain is bounded by the face (more is an invalid vkCreateImage, VUID-...-00958, which
        // VulkanImageCube now refuses with the numbers). A caller asking for a longer chain than its own
        // face supports is a size/mips disagreement — say so and proceed with the legal chain rather than
        // hand the refusal to every scene as a black environment. (PBR reads the count via
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

        // One dispatch per mip, each convolved with the matching GGX roughness.
        for ( uint32_t mip = 0; mip < mips; ++mip )
        {
            const float    roughness = ( mips > 1 ) ? static_cast<float>( mip ) / static_cast<float>( mips - 1 )
                                                     : 0.0f;
            // Dispatch over the FACE size at this mip (the shader writes per-face and clamps to imageSize).
            const uint32_t mipSize   = std::max( 1u, faceSize >> mip );
            const uint32_t groups    = DispatchGroupCount( mipSize, kComputeImagesWorkGroupSize );

            pipeline->SetInput( 0, radiance );
            pipeline->SetOutput( 1, output.get(), mip );
            pipeline->SetPushConstants( &roughness, sizeof( float ) );
            pipeline->Dispatch( groups, groups, 6u );
        }

        return output;
    }

} // namespace Desert::Graphic
