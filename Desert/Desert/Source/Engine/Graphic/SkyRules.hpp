#pragma once

#include <Engine/Assets/Serialization/EnvironmentStaging.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>

#include <Common/Core/Units.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace Desert::Graphic
{
    // The DECISIONS the sky makes, as PURE MATH — no renderer, no GPU, no state.
    //
    // The editor cannot run in the build environment, so anything that lives inside SkyboxRenderer can only
    // be checked by looking at a picture. Everything here is a function of numbers instead, and every rule
    // below is pinned by Tests/Engine/SkyRules. The renderer's job is to fetch the arguments.

    // ---------------------------------------------------------------------------------------------------
    // Which sky the Sky pass draws
    // ---------------------------------------------------------------------------------------------------

    enum class SkyMode : uint8_t
    {
        None,       // nothing to draw; the previously baked environment is retained
        Atmosphere, // the procedural gradient sky (SkyAtmosphereComponent)
        HdrCubemap  // an HDR asset (SkyboxComponent)
    };

    // The old `Procedural` bool did double duty: "which pass" AND "is the sky on". Splitting the components
    // split that decision, so it has to be stated somewhere — otherwise both pipelines think they own the
    // frame and the one that runs last wins by accident.
    inline SkyMode ResolveSkyMode( bool hasEnabledAtmosphere, bool hasHdrSkybox )
    {
        if ( hasEnabledAtmosphere )
            return SkyMode::Atmosphere;
        if ( hasHdrSkybox )
            return SkyMode::HdrCubemap;
        return SkyMode::None;
    }

    // Which of several sky entities drives the frame. Entity ids, lowest wins.
    //
    // The collector used to take whichever entity `entt` visited first and `break`. That order is not
    // stable across a component add/remove, so a scene with two skies rendered a different one after an
    // unrelated edit — with no message anywhere. Lowest id is arbitrary but REPRODUCIBLE, which is the
    // property that matters, and the caller names every candidate in the log.
    inline std::optional<size_t> SelectPrimarySky( std::span<const uint64_t> skyEntityIds )
    {
        std::optional<size_t> best;
        for ( size_t i = 0; i < skyEntityIds.size(); ++i )
        {
            if ( !best || skyEntityIds[i] < skyEntityIds[*best] )
                best = i;
        }
        return best;
    }

    // Time of day -> sun direction: Engine/Graphic/SunPosition.hpp (UE SunPosition's calendar).

    // ---------------------------------------------------------------------------------------------------
    // When the sky IBL is re-baked
    // ---------------------------------------------------------------------------------------------------

    // Baking idles the whole device and rebuilds four GPU images, so it cannot run per frame — but with the
    // time-of-day driver the sun moves EVERY frame, and the previous rule ("first enable or the button")
    // would have left the environment frozen at whatever hour the scene was opened at. The threshold is the
    // throttle, and it is a number a test can pin instead of a frame count nobody can reason about.
    //
    // @p bakedSunDir / @p currentSunDir are toward-sun directions; they need not be normalized.
    //
    // THE SUN IS NOT THE ONLY THING THE PANORAMA SEES. Since the clouds are marched into it
    // (Programs/Compute/BakeProceduralSky.shader), a sky whose sun has not moved can still be a different
    // sky: coverage, cloud type, density, extinction and albedo all change what the dome radiates.
    // @p bakedCloudFingerprint / @p currentCloudFingerprint are Graphic::CloudEnvironmentFingerprint of
    // the field the panorama on the device was baked from and of this frame's, and 0 means "no clouds" —
    // so deleting the layer rebakes exactly as adding it does. Wind and the modelling region's origin are
    // deliberately absent from that number; the reason is written where it is computed.
    //
    // NOR IS THE SUN THE ONLY THING ABOUT THE SKY ITSELF. Coverage was the second key; the sky's OWN
    // parameters are the third, and they were missing until Г11. Ground albedo, the medium, the model
    // switch, the whole artistic palette, the panorama's resolution — every one of them changes what the
    // dome radiates, none of them moves the sun, and with only the first two keys the environment stayed
    // baked from whatever sky was there before. The visible form: the sky pixels change the instant a knob
    // moves (they are marched every frame) and the light on the ground does not. It is also what a scene
    // LOAD looks like from the renderer's side — a different sky arriving with the sun in much the same
    // place — which is why this key is what makes reusing a renderer across scenes correct.
    // @p bakedSkyFingerprint / @p currentSkyFingerprint are Graphic::SkyBakeFingerprint; see it for why
    // the sun's direction is the one thing it leaves out.
    // WHETHER THE ENVIRONMENT BAKE WAITS FOR THIS VIEW'S CLOUDS. A panorama baked while the cloud layer's
    // inputs are still arriving is re-baked the moment they land: on Clouds_Protocol that was three bakes
    // at startup (sky-only 372 ms, clouds 477 ms, clouds with sky occlusion 461 ms) where one had all the
    // answers. Only the Bake button overrides it, because that is a question the user asked now.
    // @p cloudInputsPending is CloudEnvironmentBake::InputsPending; a layer whose resources FAILED is not
    // pending, so it can never hold the sky back forever.
    inline bool SkyEnvironmentBakeWaitsForClouds( bool explicitRequest, bool cloudInputsPending )
    {
        return !explicitRequest && cloudInputsPending;
    }

    // WHETHER A SKY LIGHT CAPTURE IS RECORDED WHOLE, IN THE FRAME THAT BEGINS IT (UE: the first real-time sky
    // light capture is complete; only the ones after it are time-sliced). Whole when the scene has no sky
    // light yet (a scene opened or loaded, its first view) and when this capture is the FIRST to carry the
    // clouds: a cloud layer's inputs arrive a few frames after the scene opens (SkyEnvironmentBakeWaitsForClouds
    // does not hold the very first capture back, an unlit world being the worse answer), and a time-sliced
    // capture of them left the scene lit by the bare sky for 54 frames on Clouds_Protocol -- the ambient
    // jumped when they landed. Whole, that frame pays the six faces at the screen's march once (250-300 ms on
    // an M1 Pro) and the scene's light carries its clouds from then on. Every later capture -- the sun, the
    // clouds or the sky changed -- is time-sliced while the previous, complete one lights the scene.
    // @p environmentHasClouds is whether the environment the scene reads now was captured with a marched
    // cloud layer; @p captureHasClouds whether the capture about to begin marches one.
    inline bool SkyCaptureIsWhole( bool hasEnvironment, bool environmentHasClouds, bool captureHasClouds )
    {
        return !hasEnvironment || ( captureHasClouds && !environmentHasClouds );
    }

    inline bool ShouldRebakeSkyEnvironment( const glm::vec3& bakedSunDir, const glm::vec3& currentSunDir,
                                            float thresholdDeg, bool autoRebake, bool hasEnvironment,
                                            bool explicitRequest, uint64_t bakedCloudFingerprint,
                                            uint64_t currentCloudFingerprint, uint64_t bakedSkyFingerprint,
                                            uint64_t currentSkyFingerprint )
    {
        if ( explicitRequest )
            return true;

        // The FIRST bake is not an automatic rebake: without it the scene has no ambient light at all, and
        // "Auto Rebake off" is a request to stop re-baking, not a request to render an unlit world.
        if ( !hasEnvironment )
            return true;

        if ( !autoRebake )
            return false;

        // Checked BEFORE the sun, because they are exact where the sun's test is a threshold: the clouds
        // and the sky either are the ones that were baked or they are not, and an artist dragging Coverage
        // or Ground Albedo while the sun stands still would otherwise see nothing happen.
        if ( bakedCloudFingerprint != currentCloudFingerprint )
            return true;

        if ( bakedSkyFingerprint != currentSkyFingerprint )
            return true;

        const float baked   = glm::length( bakedSunDir );
        const float current = glm::length( currentSunDir );
        if ( baked <= 0.0f || current <= 0.0f )
            return true; // no usable baked direction to compare against

        // Clamped before acos: antipodal directions land on exactly -1 - 1e-7 in float and acos() of that
        // is NaN, which compares false against every threshold and silently disables rebaking forever.
        const float cosAngle = glm::clamp( glm::dot( bakedSunDir / baked, currentSunDir / current ), -1.0f, 1.0f );
        return glm::degrees( std::acos( cosAngle ) ) > thresholdDeg;
    }

    // How long the sun must hold STILL before a wanted rebake is allowed to run.
    inline constexpr float kSkyRebakeSettleSeconds = 0.15f;
    // ...and the longest a wanted rebake may be held back while the sun keeps moving. Without this second
    // bound a sun that never stops — which is exactly what the time-of-day driver does — would defer the
    // bake forever and freeze the environment at the hour the scene was opened at.
    inline constexpr float kSkyRebakeMaxDeferSeconds = 1.0f;

    // Whether a rebake that ShouldRebakeSkyEnvironment has already ASKED for may run this frame.
    //
    // The angular threshold decides that the environment is stale; this decides when to act on it. They
    // are different questions, and conflating them is what made dragging the sun unusable: at 5 degrees
    // per step a drag crosses the threshold several times a second, and every crossing idled the device
    // and rebuilt four cube images. Waiting for the drag to END collapses that whole gesture into one
    // bake, and the deferral bound keeps a continuously moving sun refreshing at ~1 Hz regardless.
    //
    // @p secondsSinceSunMoved counts from the last frame the sun direction actually changed;
    // @p secondsSinceStale counts from the frame the rebake was first wanted.
    inline bool SkyEnvironmentRebakeMayRun( float secondsSinceSunMoved, float secondsSinceStale,
                                            float settleSeconds, float maxDeferSeconds )
    {
        return secondsSinceSunMoved >= settleSeconds || secondsSinceStale >= maxDeferSeconds;
    }

    // ---------------------------------------------------------------------------------------------------
    // What the bake costs
    // ---------------------------------------------------------------------------------------------------

    // The IBL cube chain's sizes live beside the cache that keys on them (`EnvironmentStaging.hpp`), because the
    // asset layer stages the cubes on a worker and must not include the renderer to learn their shape.
    using Assets::kSkyEnvCubeFaceSize;
    using Assets::kSkyEnvIrradianceFaceSize;
    using Assets::kSkyEnvIrradianceSourceFace;
    using Assets::kSkyEnvIrradianceSourceMip;
    using Assets::kSkyEnvPrefilterFaceSize;
    using Assets::kSkyEnvPrefilterMips;
    using Assets::kSkyEnvRadianceMips;

    // The format every one of the three cubes is created at — `ComputeImages::ProccessForImageCube` and
    // `ProccessForImageCubeMips` both ask for RGBA32F. It is the FORMAT that is written down here, not
    // its size: `kSkyEnvBytesPerPixel = 16` used to sit on this line, a hand-typed copy of
    // `GetBytesPerPixel( RGBA32F )` that nothing made agree with it and that would have gone on
    // answering 16 on the day the cubes moved to RGBA16F.
    inline constexpr Core::Formats::ImageFormat kSkyEnvCubeFormat = Core::Formats::ImageFormat::RGBA32F;

    /// THE SKY LIGHT'S CAPTURE SIZE: one cube FACE. The sky and its clouds are rendered straight into the faces
    /// of the radiance cube at this size (UE: FSkyLightSceneProxy::CaptureResolution, 128 by default, the
    /// real-time capture renders the sky material and the volumetric clouds per face at it); the prefiltered
    /// specular cube is the same face, and the irradiance is summed over its 16-texel mip. There is no panorama
    /// any more.
    struct SkyEnvironmentSize
    {
        uint32_t Face = 0;
    };

    // An ENUM, not an int: a face that is not a power of two has no 16-texel mip to sum the irradiance over, and
    // one under 32 would leave the capture's 8x8 groups half empty.
    inline SkyEnvironmentSize EnvironmentCaptureSize( ECS::SkyEnvironmentResolution resolution )
    {
        switch ( resolution )
        {
            case ECS::SkyEnvironmentResolution::Low:
                return { 64u };
            case ECS::SkyEnvironmentResolution::High:
                return { 256u };
            case ECS::SkyEnvironmentResolution::Medium:
                break;
        }
        return { 128u };
    }

    /// The mip of a capture's radiance cube whose face is kSkyEnvIrradianceSourceFace: the one the irradiance is
    /// summed over exactly, the same 16-texel face the .hdr bake reads at its own kSkyEnvIrradianceSourceMip.
    inline constexpr uint32_t SkyCaptureIrradianceSourceMip( uint32_t face )
    {
        return static_cast<uint32_t>( std::countr_zero( face ) - std::countr_zero( kSkyEnvIrradianceSourceFace ) );
    }

    inline const char* EnvironmentResolutionName( ECS::SkyEnvironmentResolution resolution )
    {
        switch ( resolution )
        {
            case ECS::SkyEnvironmentResolution::Low:
                return "Low";
            case ECS::SkyEnvironmentResolution::High:
                return "High";
            case ECS::SkyEnvironmentResolution::Medium:
                break;
        }
        return "Medium";
    }

    // Bytes a cube of @p faceSize with @p mips levels occupies (6 faces, halved per mip, RGBA32F).
    // THE SIX AND THE SIXTEEN ARE BOTH GONE FROM THIS FUNCTION. It used to open-code both, which made it
    // the only place in the engine that knew what a cube costs and the only place that could be wrong
    // about it on its own. Both now come from `Core/Formats/ImageFormat.hpp`, beside the format table
    // that has to answer for them.
    inline uint64_t SkyEnvironmentCubeBytes( uint32_t faceSize, uint32_t mips )
    {
        return Core::Formats::CalculateCubeImageSize( faceSize, mips, kSkyEnvCubeFormat );
    }

    struct SkyEnvironmentCost
    {
        uint64_t RadianceBytes = 0; // the capture's intermediate, released when it completes
        uint64_t CubeBytes     = 0; // irradiance + prefiltered, the two the scene keeps
        uint64_t TotalBytes    = 0;
    };

    // What one captured sky light costs on the GPU while it is being captured. Each addend is (face, mips)
    // EXACTLY as SkyCapture::Begin creates it, from the same functions; Tests/Engine/SkyRules pins the agreement.
    inline SkyEnvironmentCost SkyEnvironmentBakeCost( ECS::SkyEnvironmentResolution resolution )
    {
        const uint32_t face = EnvironmentCaptureSize( resolution ).Face;
        const uint32_t mips = Core::Formats::MipChainLength( face );

        SkyEnvironmentCost cost;
        cost.RadianceBytes = SkyEnvironmentCubeBytes( face, mips );
        cost.CubeBytes =
             SkyEnvironmentCubeBytes( kSkyEnvIrradianceFaceSize, 1u ) + SkyEnvironmentCubeBytes( face, mips );
        cost.TotalBytes = cost.RadianceBytes + cost.CubeBytes;
        return cost;
    }

    inline double BytesToMiB( uint64_t bytes )
    {
        return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
    }

    // ---------------------------------------------------------------------------------------------------
    // Planet radius
    // ---------------------------------------------------------------------------------------------------

    // The component authors kilometres because 6360 is a number a human can check and 636000000 is not.
    // This is the ONE place the conversion to world units (centimetres) happens.
    inline float PlanetRadiusToWorldUnits( float kilometres )
    {
        return Common::Units::Metres( kilometres * 1000.0f );
    }
} // namespace Desert::Graphic
