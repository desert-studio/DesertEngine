#pragma once

#include "Image.hpp"
#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Runtime/ImageHandle.hpp>

#include <glm/glm.hpp>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    // Must match the conversion shaders' LocalSize(32, 32, 1) — a dispatch derived with a different
    // group size under-covers the output and the right/bottom edge is never written.
    inline constexpr uint32_t kComputeImagesWorkGroupSize = 32u;

    // Whole workgroups covering @p extentTexels threads: the pair (extent, groups) this returns is the
    // relation Tests/Engine/SkyRules pins — every texel covered, no whole surplus group. Deriving the
    // extent from anything but the output's own face is how one bake dispatched 12x its texel count.
    constexpr uint32_t DispatchGroupCount( uint32_t extentTexels, uint32_t localSize )
    {
        return ( extentTexels + localSize - 1u ) / localSize;
    }

    struct ComputeImagesSpecification
    {
        Runtime::ImageHandle InputHandle;
        std::string          Tag;
        std::string          ShaderName;
        uint32_t             MipLevels;
        // The FACE edge of the output cube, in texels — the same quantity ImageCubeSpecification::FaceSize
        // names. Callers state the face they want; nothing downstream multiplies or divides by the 4x3
        // cross unwrap any more (that arithmetic produced three defects — see ImageCubeSpecification).
        uint32_t FaceSize;
    };

    class ComputeImages final
    {
    public:
        virtual ~ComputeImages() = default;

        static std::shared_ptr<Image2D> ProccessForImage2D( const std::shared_ptr<Image>& image );
        // Bakes the procedural atmosphere into an equirect HDR panorama (RGBA32F, Storage|Sample). No input
        // image: the sky is generated in-shader from @p skyParams, which is the SAME buffer the screen sky
        // pass reads, so the baked lighting and the visible sky cannot describe different skies.
        // @p transmittanceLut / @p multiScatterLut feed the physical model's march; nullptr (the gradient
        // model) binds the engine fallbacks so the shader's samplers are never undefined descriptors.
        // @p clouds is this view's cloud layer, marched into the same panorama by the same field the
        // screen pass marches — see Engine/Graphic/Clouds/CloudEnvironmentBake.hpp. Its Marched flag is
        // the only gate: every descriptor the shader declares is written on every path, because an
        // unbound one makes the set invalid and this backend answers an invalid set by skipping the whole
        // dispatch — which here would lose the environment, not the clouds, with nothing in the log.
        static std::shared_ptr<Image2D> BakeProceduralPanorama( uint32_t width, uint32_t height,
                                                                ShaderResources::StorageBuffer* skyParams,
                                                                Image2D*                        transmittanceLut,
                                                                Image2D*                        multiScatterLut,
                                                                const CloudBakeBinding&         clouds );
        // Single dispatch: samples spec.InputHandle (2D panorama OR source cubemap) -> a fresh output cube.
        static std::shared_ptr<ImageCube> ProccessForImageCube( const ComputeImagesSpecification& spec );
        // GGX prefilter: convolves spec.InputHandle (radiance cube) per mip (roughness = mip/(mips-1))
        // into a mipped output cube. Returns the prefiltered cube.
        static std::shared_ptr<ImageCube> ProccessForImageCubeMips( const ComputeImagesSpecification& spec );
    };

} // namespace Desert::Graphic