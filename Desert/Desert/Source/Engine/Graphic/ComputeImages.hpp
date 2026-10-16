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
    class GpuBatch;

    // Must match the conversion shaders' LocalSize(32, 32, 1) — a dispatch derived with a different
    // group size under-covers the output and the right/bottom edge is never written.
    inline constexpr uint32_t kComputeImagesWorkGroupSize = 32u;
    // The cube bakes' (PanoramaToCubemap, DiffuseIrradiance, PrefilterEnvMap) set-0 map: the source as a separate
    // texture at 0, the output storage image at 1, and the one shared state the source is read through at 2.
    // That state is trilinear REPEAT and not the global texture filter: a bake is cached by its inputs, and a
    // display setting must not change what it bakes. REPEAT wraps the panorama's longitude; a cube ignores it.
    inline constexpr uint32_t kComputeImagesInputBinding   = 0u;
    inline constexpr uint32_t kComputeImagesOutputBinding  = 1u;
    inline constexpr uint32_t kComputeImagesSamplerBinding = 2u;

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
        // Single dispatch: samples spec.InputHandle (2D panorama OR source cubemap) -> a fresh output cube.
        // Both RECORD into @p batch and return the cube before the GPU has written it: the caller submits
        // the batch and must not sample the cube until it completes. The transient pipeline is retained
        // by the batch; the input image is the caller's to keep alive.
        static std::shared_ptr<ImageCube> ProccessForImageCube( GpuBatch&                         batch,
                                                                const ComputeImagesSpecification& spec );
        // GGX prefilter: convolves spec.InputHandle (radiance cube) per mip (roughness = mip/(mips-1))
        // into a mipped output cube. Returns the prefiltered cube.
        static std::shared_ptr<ImageCube> ProccessForImageCubeMips( GpuBatch&                         batch,
                                                                    const ComputeImagesSpecification& spec );
        // Diffuse irradiance: the exact cosine-weighted sum over one mip of spec.InputHandle (a radiance cube
        // with its chain), @p sourceMip, into a one-mip cube of spec.FaceSize -- SkyCaptureIrradiance, the same
        // program and source mip the procedural sky's capture records in the frame graph.
        static std::shared_ptr<ImageCube>
        IntegrateIrradianceCube( GpuBatch& batch, const ComputeImagesSpecification& spec, uint32_t sourceMip );
    };

} // namespace Desert::Graphic