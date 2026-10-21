#pragma once

#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Runtime/ImageHandle.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class GpuBatch;

    struct Environment
    {
        // What the cubes were baked from: the source .hdr path, or "ProceduralSky" for the procedural bake.
        // A record of the source, not a handle anything reloads the environment by.
        Common::Filepath Filepath;

        // THE SHARP CUBE, AND IT IS NOT ALWAYS THERE. Only the .hdr path keeps one: it is what the
        // skybox pass draws as the backdrop and what the two editor previews read. The procedural sky
        // marches its backdrop fullscreen and never samples a cube, so its capture (SkyCapture) frees this one
        // once the prefilter has consumed it (128 MiB per environment) and leaves the handle empty.
        // Resolving an empty handle answers nullptr, never another image — ImageService::Resolve is
        // generation-checked — so a consumer that forgets to ask gets nothing rather than the wrong sky.
        Runtime::ImageHandle RadianceMap;
        Runtime::ImageHandle IrradianceMap;
        Runtime::ImageHandle PreFilteredMap;

        /// HOW THE THREE CUBES ARE TO BE READ — the scene's rotation and gain, applied at every sample
        /// (Shaders/Common/SkyLook.glslh). Identity on everything the environment services build; only
        /// `SkyboxRenderer::GetEnvironment` sets it, from the look the scene asked for, so the backdrop,
        /// the forward materials and the deferred composite all read the one value it composed.
        SkyLook Look{};

        /// "This environment can light and reflect the scene", which is the only question its eight
        /// callers ask — a failed bake, a previous environment worth releasing, a skybox worth previewing.
        ///
        /// THE RADIANCE CUBE IS NOT PART OF THE ANSWER ANY MORE, and leaving it in would have been the
        /// quiet half of freeing it: `SkyboxRenderer::GetEnvironment` gates on this operator, so a
        /// procedural environment carrying an empty radiance handle would have reported itself as NO
        /// ENVIRONMENT — taking every scene's ambient and reflections away while the sky still drew,
        /// and the bake would have re-run every frame because `EnsureProceduralEnvironment` reads the
        /// same bit as "not baked yet". The backdrop is asked for by name where it is needed
        /// (`MaterialSkybox::BindInputs` and both editor previews all test `RadianceMap.IsValid()`).
        operator bool() const
        {
            return IrradianceMap.IsValid() && PreFilteredMap.IsValid();
        }
    };

    /// WHICH BAKE an environment is: its prefiltered cube's handle (slot and generation, so a rebake into a reused
    /// slot is another bake), 0 for none or a half-built one. The look is not part of it (a rotation or a gain is
    /// applied at every sample and rebakes nothing).
    [[nodiscard]] inline uint64_t EnvironmentIdentity( const std::optional<Environment>& environment )
    {
        if ( environment.has_value() && environment->PreFilteredMap.IsValid() )
            return ( static_cast<uint64_t>( environment->PreFilteredMap.Value.Index ) << 32u ) |
                   environment->PreFilteredMap.Value.Generation;
        return 0;
    }

    class EnvironmentManager
    {
    public:
        // Builds the three IBL cubes of an HDR skybox asset — the panorama as authored, at unit gain. The
        // scene's look (rotation, tint, intensity) is NOT in them: it is applied where they are sampled
        // (Environment/SkyLook.hpp), which is what keeps a slider drag from being a bake per value.
        //
        // ON A MISS THE CUBES ARE NOT WRITTEN YET WHEN THIS RETURNS. The whole chain is recorded into one
        // batch, submitted, and handed back through @p convolving still running on the GPU; the caller keeps
        // it until `IsComplete()` and samples nothing before then (SkyboxService holds the skybox pending
        // meanwhile). A hit, and every refusal, leaves @p convolving empty.
        static Environment Create( const std::shared_ptr<Assets::SkyboxAsset>& skyboxAsset,
                                   std::unique_ptr<GpuBatch>&                  convolving );

        // THE PROCEDURAL SKY IS NOT BUILT HERE. Its light is captured in the frame graph of the view that
        // needs it (Systems/Scene/Skybox/SkyCapture), with the same three programs this class runs on a file:
        // a sky whose input is a time of day has no file to bake, and a capture outside the frame left every new
        // view without sky ambient until a fence signalled.

    private:
        // Samples an equirect panorama into the radiance cube (the sharp environment the skybox draws and
        // the prefilter convolves). Named for the RESULT: the 4x3 "cross" this used to be named after was
        // an internal unwrap of the source pixels, and carrying it in the name is how call sites came to
        // reason in cross widths instead of faces.
        static std::shared_ptr<ImageCube> ConvertPanoramaToRadianceCube( GpuBatch&                   batch,
                                                                         const Runtime::ImageHandle& panorama );

        // The diffuse irradiance, summed exactly over the radiance cube's 16-face mip (SkyCaptureIrradiance).
        static std::shared_ptr<ImageCube> CreateDiffuseIrradiance( GpuBatch&                   batch,
                                                                   const Runtime::ImageHandle& radianceCube );

        // GGX-prefilters an already-built radiance cubemap (per-mip roughness).
        static std::shared_ptr<ImageCube> CreatePrefilteredMap( GpuBatch&                   batch,
                                                                const Runtime::ImageHandle& radianceCube );
    };
} // namespace Desert::Graphic