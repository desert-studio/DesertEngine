#pragma once

#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Runtime/ImageHandle.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <optional>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
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

    /// A SPECIFIED CUBEMAP THE CACHE DID NOT HOLD: its panorama, converted and filtered in the frame graph of the
    /// first view that draws it (System::SkyCapture::BeginPanorama, MaterialSkybox::AddConvolutionPasses), and
    /// the three cache files that conversion is then written to (radiance, irradiance, prefilter; Live is set
    /// by EnvironmentManager::WriteCache from the converted environment).
    struct EnvironmentConvolution
    {
        std::shared_ptr<Texture2D>           Panorama;
        std::array<EnvironmentCacheEntry, 3> Cache;
    };

    class EnvironmentManager
    {
    public:
        // The three IBL cubes of an HDR skybox asset -- the panorama as authored, at unit gain. The scene's look
        // (rotation, tint, intensity) is NOT in them: it is applied where they are sampled
        // (Environment/SkyLook.hpp), which is what keeps a slider drag from being a bake per value.
        //
        // A HIT uploads the cached cubes and returns them. A MISS returns an environment with no cubes and hands
        // the panorama back through @p convolution: the conversion is frame-graph work (SkyCapture), recorded by
        // the first view that draws the skybox, whole, ahead of everything that samples it -- the frame that
        // records it is lit by it. Every refusal leaves @p convolution empty and is logged.
        static Environment Create( const std::shared_ptr<Assets::SkyboxAsset>& skyboxAsset,
                                   std::optional<EnvironmentConvolution>&      convolution );

        // Writes a converted environment to its cache files. Called once the graph that converted it has been
        // submitted (the writer's readbacks go after it on the same queue); a failure costs the cache only, and
        // is logged with the file.
        static void WriteCache( EnvironmentConvolution& convolution, const Environment& converted );

        // THE PROCEDURAL SKY IS NOT BUILT HERE either. Its light is captured in the frame graph of the view that
        // needs it (Systems/Scene/Skybox/SkyCapture), with the same programs a specified cubemap is converted
        // with.
    };
} // namespace Desert::Graphic