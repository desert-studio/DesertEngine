#pragma once

#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/ImageHandle.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

#include <glm/glm.hpp>

#include <memory>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class GpuBatch;

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

    /// A PROCEDURAL SKY BAKE IN FLIGHT: the panorama, radiance, irradiance and prefiltered cubes recorded into
    /// one batch and submitted, not waited for. The owner polls `IsComplete()` and calls `Finish()` once —
    /// which drops the two intermediates (panorama, radiance cube) and hands over the environment that
    /// lights the scene. Dropped unfinished, it waits for its batch and releases all four images, so an
    /// abandoned bake (a renderer closed mid-bake) leaks nothing and frees nothing the GPU still reads.
    class ProceduralEnvironmentBake
    {
    public:
        ProceduralEnvironmentBake() = default;
        ~ProceduralEnvironmentBake();
        ProceduralEnvironmentBake( const ProceduralEnvironmentBake& )            = delete;
        ProceduralEnvironmentBake& operator=( const ProceduralEnvironmentBake& ) = delete;

        /// True once the GPU has written every cube. Never blocks.
        [[nodiscard]] bool IsComplete() const;
        /// Blocks until the GPU has written every cube.
        void Wait();
        /// Called once, after `IsComplete()`: releases the intermediates and returns the environment, whose
        /// two cubes the caller now owns. Before completion it is refused with an empty environment.
        [[nodiscard]] Environment Finish();

    private:
        friend class EnvironmentManager;
        void ReleaseAll();

        std::unique_ptr<GpuBatch> m_Batch;
        Runtime::ImageHandle      m_Panorama;
        Runtime::ImageHandle      m_Radiance;
        Runtime::ImageHandle      m_Irradiance;
        Runtime::ImageHandle      m_Prefiltered;
    };

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

        // THE SAME BAKE, NOT WAITED FOR: everything above recorded into one batch and submitted, the bake
        // handed back still running. What the renderer uses — the main thread never sleeps on the GPU for it.
        // `CreateProcedural` is this plus `Wait` plus `Finish`, for a caller that needs the cubes on return.
        // nullptr when nothing could be recorded (the reason is logged).
        static std::unique_ptr<ProceduralEnvironmentBake>
        BeginProcedural( uint32_t panoramaWidth, uint32_t panoramaHeight, ShaderResources::StorageBuffer* skyParams,
                         Image2D* transmittanceLut, Image2D* multiScatterLut, const CloudBakeBinding& clouds );

    private:
        // Samples an equirect panorama into the radiance cube (the sharp environment the skybox draws and
        // the prefilter convolves). Named for the RESULT: the 4x3 "cross" this used to be named after was
        // an internal unwrap of the source pixels, and carrying it in the name is how call sites came to
        // reason in cross widths instead of faces.
        static std::shared_ptr<ImageCube> ConvertPanoramaToRadianceCube( GpuBatch&                   batch,
                                                                         const Runtime::ImageHandle& panorama );

        static std::shared_ptr<ImageCube> CreateDiffuseIrradiance( GpuBatch&                   batch,
                                                                   const Runtime::ImageHandle& panorama );

        // GGX-prefilters an already-built radiance cubemap (per-mip roughness).
        static std::shared_ptr<ImageCube> CreatePrefilteredMap( GpuBatch&                   batch,
                                                                const Runtime::ImageHandle& radianceCube );
    };
} // namespace Desert::Graphic