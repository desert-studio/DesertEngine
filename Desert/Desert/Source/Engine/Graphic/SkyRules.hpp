#pragma once

#include <Engine/Assets/Serialization/EnvironmentStaging.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>

#include <Common/Core/Units.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
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

    // ---------------------------------------------------------------------------------------------------
    // Time of day -> sun direction
    // ---------------------------------------------------------------------------------------------------

    // Where the sun is at @p hours of solar time, as the direction the sunlight TRAVELS (sun -> scene) —
    // which is exactly what a directional light's TransformComponent::Translation encodes.
    //
    // Horizon frame: +X east, +Y up, +Z north. Solar declination is ZERO: the model has no calendar, so
    // this is the equinox path — the sun rises due east, sets due west, and @p latitudeDeg only decides
    // how high noon gets. @p northOffsetDeg rotates solar north onto the scene's north about world +Y, in
    // DEGREES converted here (the reference implementation this model comes from fed 45 degrees into a
    // radians parameter and got a sun path nobody could explain).
    inline glm::vec3 SunDirectionFromTimeOfDay( float hours, float latitudeDeg, float northOffsetDeg )
    {
        // Hour angle: 0 at noon, -pi at midnight, +-pi/2 at 06:00 / 18:00.
        const float hourAngle = ( hours / 24.0f ) * glm::two_pi<float>() - glm::pi<float>();
        const float latitude  = glm::radians( latitudeDeg );

        const float sinH = std::sin( hourAngle );
        const float cosH = std::cos( hourAngle );

        // Already unit length: sin^2(H) + cos^2(H) * (cos^2(lat) + sin^2(lat)) == 1.
        const glm::vec3 towardSunSolar( -sinH, std::cos( latitude ) * cosH, -std::sin( latitude ) * cosH );

        const glm::vec3 towardSun = glm::vec3(
             glm::rotate( glm::mat4( 1.0f ), glm::radians( northOffsetDeg ), glm::vec3( 0.0f, 1.0f, 0.0f ) ) *
             glm::vec4( towardSunSolar, 0.0f ) );

        return -towardSun;
    }

    // Advances the clock by one frame. @p dayLengthSeconds == 0 freezes the sun at the authored hour, which
    // is what makes "Drive Sun From Time Of Day" usable as a posing tool and not only as an animation.
    inline float AdvanceTimeOfDay( float hours, float dtSeconds, float dayLengthSeconds )
    {
        if ( dayLengthSeconds <= 0.0f )
            return hours;

        float next = hours + dtSeconds * 24.0f / dayLengthSeconds;
        next       = std::fmod( next, 24.0f );
        if ( next < 0.0f )
            next += 24.0f;
        return next;
    }

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
    using Assets::kSkyEnvPrefilterFaceSize;
    using Assets::kSkyEnvPrefilterMips;
    using Assets::kSkyEnvRadianceMips;

    // The format every one of the three cubes is created at — `ComputeImages::ProccessForImageCube` and
    // `ProccessForImageCubeMips` both ask for RGBA32F. It is the FORMAT that is written down here, not
    // its size: `kSkyEnvBytesPerPixel = 16` used to sit on this line, a hand-typed copy of
    // `GetBytesPerPixel( RGBA32F )` that nothing made agree with it and that would have gone on
    // answering 16 on the day the cubes moved to RGBA16F.
    inline constexpr Core::Formats::ImageFormat kSkyEnvCubeFormat = Core::Formats::ImageFormat::RGBA32F;

    struct SkyEnvironmentSize
    {
        uint32_t Width  = 0;
        uint32_t Height = 0;
    };

    // An ENUM, not an int: the bake dispatches in 32x32 work groups, so a hand-typed size that is not a
    // multiple of 32 would silently leave the panorama's right/bottom edge unwritten.
    inline SkyEnvironmentSize EnvironmentPanoramaSize( ECS::SkyEnvironmentResolution resolution )
    {
        switch ( resolution )
        {
            case ECS::SkyEnvironmentResolution::Low:
                return { 512u, 256u };
            case ECS::SkyEnvironmentResolution::High:
                return { 2048u, 1024u };
            case ECS::SkyEnvironmentResolution::Medium:
                break;
        }
        return { 1024u, 512u };
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
        uint64_t PanoramaBytes = 0;
        uint64_t CubeBytes     = 0; // radiance + irradiance + prefiltered
        uint64_t TotalBytes    = 0;
    };

    // What one baked environment costs on the GPU. Note that only the PANORAMA scales with the resolution
    // ladder — the cube chain's faces are fixed constants either way — which is exactly the kind of thing
    // a number in the log tells you and a tooltip does not.
    //
    // Each addend below is (face, mips) EXACTLY as SceneEnvironment requests it, from the same constants.
    // This function once charged for a 1024-face 11-mip prefiltered cube (128 MiB) while the bake built a
    // 256-face 9-mip one (8 MiB) — the two sides shared the face constant but one call site multiplied it
    // by the cross layout and the other did not. Tests/Engine/SkyRules pins the agreement.
    inline SkyEnvironmentCost SkyEnvironmentBakeCost( ECS::SkyEnvironmentResolution resolution )
    {
        const SkyEnvironmentSize size = EnvironmentPanoramaSize( resolution );

        SkyEnvironmentCost cost;
        cost.PanoramaBytes = Core::Formats::CalculateImageSize( size.Width, size.Height, kSkyEnvCubeFormat );
        cost.CubeBytes     = SkyEnvironmentCubeBytes( kSkyEnvCubeFaceSize, kSkyEnvRadianceMips ) +
                         SkyEnvironmentCubeBytes( kSkyEnvIrradianceFaceSize, 1u ) +
                         SkyEnvironmentCubeBytes( kSkyEnvPrefilterFaceSize, kSkyEnvPrefilterMips );
        cost.TotalBytes = cost.PanoramaBytes + cost.CubeBytes;
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
