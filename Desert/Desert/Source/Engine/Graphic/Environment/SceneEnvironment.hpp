#pragma once

#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/ImageHandle.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

#include <glm/glm.hpp>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    struct Environment
    {
        Common::Filepath Filepath; // TODO: Asset Env

        // THE SHARP CUBE, AND IT IS NOT ALWAYS THERE. Only the .hdr path keeps one: it is what the
        // skybox pass draws as the backdrop and what the two editor previews read. The procedural sky
        // marches its backdrop fullscreen and never samples a cube, so `CreateProcedural` frees this one
        // the moment the prefilter has consumed it (96 MiB per environment) and leaves the handle empty.
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

    class EnvironmentManager
    {
    public:
        // Builds the three IBL cubes of an HDR skybox asset — the panorama as authored, at unit gain. The
        // scene's look (rotation, tint, intensity) is NOT in them: it is applied where they are sampled
        // (Environment/SkyLook.hpp), which is what keeps a slider drag from being a bake per value.
        static Environment Create( const std::shared_ptr<Assets::SkyboxAsset>& skyboxAsset );

        // Builds an IBL environment from the engine-generated procedural atmosphere (no HDR asset): the sky
        // is baked into an equirect panorama of @p panoramaWidth x @p panoramaHeight, then run through the
        // same radiance/irradiance/prefilter pipeline. @p skyParams is the caller's sky parameter buffer —
        // the same one the screen sky pass reads.
        //
        // @p transmittanceLut / @p multiScatterLut are the cached atmosphere LUTs the bake's physical
        // branch (SkyModel::PhysicalAtmosphere) marches with; the caller guarantees they hold valid
        // texels when the payload's model lane says physical. Pass nullptr on the gradient model — the
        // bake then binds fallbacks and the physical branch is never taken.
        //
        // @p clouds is the caller's cloud layer, marched into the SAME panorama by the SAME field the
        // screen pass marches — see Engine/Graphic/Clouds/CloudEnvironmentBake.hpp for why the argument
        // exists at all: this call is the only route from the clouds to the bake, and the alternative was
        // a second, analytic model of the clouds standing beside the march.
        //
        // The panorama size is authored (SkyAtmosphereData::EnvironmentResolution) rather than a constant
        // because this cost is paid PER LIVE SceneRenderer, and the editor keeps several of those.
        static Environment CreateProcedural( uint32_t panoramaWidth, uint32_t panoramaHeight,
                                             ShaderResources::StorageBuffer* skyParams, Image2D* transmittanceLut,
                                             Image2D* multiScatterLut, const CloudBakeBinding& clouds );

    private:
        // Samples an equirect panorama into the radiance cube (the sharp environment the skybox draws and
        // the prefilter convolves). Named for the RESULT: the 4x3 "cross" this used to be named after was
        // an internal unwrap of the source pixels, and carrying it in the name is how call sites came to
        // reason in cross widths instead of faces.
        static std::shared_ptr<ImageCube> ConvertPanoramaToRadianceCube( const Runtime::ImageHandle& panorama );

        static std::shared_ptr<ImageCube> CreateDiffuseIrradiance( const Runtime::ImageHandle& panorama );

        // GGX-prefilters an already-built radiance cubemap (per-mip roughness).
        static std::shared_ptr<ImageCube>
        CreatePrefilteredMap( const Runtime::ImageHandle& radianceCube );
    };
} // namespace Desert::Graphic