// The sky's DECISIONS, tested without a GPU.
//
// The editor cannot run in this environment, so a sky rule that lives inside SkyboxRenderer can only be
// checked by looking at a picture — which is how a dead m_BakedSunDir sat in the renderer unread, and how
// an inverted sun shipped in four scenes. Everything asserted below is a pure function of numbers:
// Engine/Graphic/SkyRules.hpp, SkyPayload.hpp, AtmosphereEnv.hpp, SkySettings.hpp.

#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Graphic/ColorTemperature.hpp>
#include <Engine/Graphic/ComputeImages.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/SkyPayload.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Graphic/SkySettings.hpp>

#include <Common/Core/Units.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>

using Desert::Core::Formats::MipChainLength;
using Desert::ECS::SkyAtmosphereData;
using Desert::ECS::SkyEnvironmentResolution;
using Desert::Graphic::AdvanceTimeOfDay;
using Desert::Graphic::BytesToMiB;
using Desert::Graphic::CloudEnvironmentFingerprint;
using Desert::Graphic::CloudGpuPayload;
using Desert::Graphic::CloudRegionBinding;
using Desert::Graphic::CloudTypeShape;
using Desert::Graphic::DispatchGroupCount;
using Desert::Graphic::EnvironmentPanoramaSize;
using Desert::Graphic::EvaluateAtmosphere;
using Desert::Graphic::kComputeImagesWorkGroupSize;
using Desert::Graphic::kSkyEnvCubeFaceSize;
using Desert::Graphic::kSkyEnvIrradianceFaceSize;
using Desert::Graphic::kSkyEnvPrefilterFaceSize;
using Desert::Graphic::kSkyEnvPrefilterMips;
using Desert::Graphic::kSkyEnvRadianceMips;
using Desert::Graphic::kSkyPackedVec4Count;
using Desert::Graphic::kSkyPayloadBytes;
using Desert::Graphic::kSkyRebakeMaxDeferSeconds;
using Desert::Graphic::kSkyRebakeSettleSeconds;
using Desert::Graphic::MakeSkySettings;
using Desert::Graphic::PackCloudParams;
using Desert::Graphic::PackSky;
using Desert::Graphic::PlanetRadiusToWorldUnits;
using Desert::Graphic::ResolveSkyMode;
using Desert::Graphic::SelectPrimarySky;
using Desert::Graphic::ShouldRebakeSkyEnvironment;
using Desert::Graphic::SkyEnvironmentBakeWaitsForClouds;
using Desert::Graphic::SkyEnvironmentBakeCost;
using Desert::Graphic::SkyEnvironmentRebakeMayRun;
using Desert::Graphic::SkyGpuPayload;
using Desert::Graphic::SkyMode;
using Desert::Graphic::SkySettings;
using Desert::Graphic::SunDirectionFromTimeOfDay;

// ---------------------------------------------------------------------------------------------------
// Time of day -> sun direction. The return value is the direction the light TRAVELS (sun -> scene), so
// a sun overhead is a NEGATIVE y. Getting that backwards gives a world lit from underneath.
// ---------------------------------------------------------------------------------------------------

TEST( TimeOfDay, NoonOnTheEquatorPutsTheSunOverhead )
{
    const glm::vec3 travel = SunDirectionFromTimeOfDay( 12.0f, 0.0f, 0.0f );
    EXPECT_NEAR( travel.y, -1.0f, 1e-4f ) << "light travels DOWN when the sun is up";
    EXPECT_NEAR( glm::length( travel ), 1.0f, 1e-5f );
}

TEST( TimeOfDay, MidnightPutsTheSunUnderTheWorld )
{
    const glm::vec3 travel = SunDirectionFromTimeOfDay( 0.0f, 0.0f, 0.0f );
    EXPECT_NEAR( travel.y, 1.0f, 1e-4f );
}

TEST( TimeOfDay, SunriseAndSunsetSitOnTheHorizon )
{
    EXPECT_NEAR( SunDirectionFromTimeOfDay( 6.0f, 0.0f, 0.0f ).y, 0.0f, 1e-3f );
    EXPECT_NEAR( SunDirectionFromTimeOfDay( 18.0f, 0.0f, 0.0f ).y, 0.0f, 1e-3f );

    // ...and on OPPOSITE sides of the sky, which is the part a sign error would still pass without.
    const glm::vec3 dawn = SunDirectionFromTimeOfDay( 6.0f, 0.0f, 0.0f );
    const glm::vec3 dusk = SunDirectionFromTimeOfDay( 18.0f, 0.0f, 0.0f );
    EXPECT_NEAR( glm::dot( dawn, dusk ), -1.0f, 1e-3f );
}

TEST( TimeOfDay, LatitudeTiltsTheNoonSunTowardTheEquator )
{
    // Northern hemisphere: noon is 45 degrees up and to the SOUTH (-Z, since +Z is north).
    const glm::vec3 towardSun = -SunDirectionFromTimeOfDay( 12.0f, 45.0f, 0.0f );
    EXPECT_NEAR( towardSun.y, std::cos( glm::radians( 45.0f ) ), 1e-4f );
    EXPECT_NEAR( towardSun.z, -std::sin( glm::radians( 45.0f ) ), 1e-4f );
    EXPECT_NEAR( towardSun.x, 0.0f, 1e-4f );

    // Southern hemisphere mirrors it to the north; the elevation is the same.
    const glm::vec3 south = -SunDirectionFromTimeOfDay( 12.0f, -45.0f, 0.0f );
    EXPECT_NEAR( south.y, towardSun.y, 1e-4f );
    EXPECT_NEAR( south.z, -towardSun.z, 1e-4f );
}

TEST( TimeOfDay, NorthOffsetRotatesTheAzimuthAboutWorldY )
{
    // Measured at latitude 45, because at the equator noon is straight up and its azimuth is undefined —
    // a test written there would pass for a function that ignored NorthOffset entirely.
    const glm::vec3 before = -SunDirectionFromTimeOfDay( 12.0f, 45.0f, 0.0f );
    const glm::vec3 after  = -SunDirectionFromTimeOfDay( 12.0f, 45.0f, 90.0f );

    EXPECT_NEAR( after.y, before.y, 1e-4f ) << "a rotation about Y cannot change the elevation";

    const glm::vec2 h0 = glm::normalize( glm::vec2( before.x, before.z ) );
    const glm::vec2 h1 = glm::normalize( glm::vec2( after.x, after.z ) );
    EXPECT_NEAR( glm::dot( h0, h1 ), 0.0f, 1e-4f ) << "90 degrees apart on the ground plane";
}

TEST( TimeOfDay, ClockAdvancesWrapsAndFreezes )
{
    // A 600-second day: one real second is 0.04 h.
    EXPECT_NEAR( AdvanceTimeOfDay( 12.0f, 1.0f, 600.0f ), 12.04f, 1e-4f );

    // Wraps at 24 rather than running off to 25.
    EXPECT_NEAR( AdvanceTimeOfDay( 23.99f, 1.0f, 600.0f ), 0.03f, 1e-3f );

    // DayLengthSeconds == 0 freezes the sun at the authored hour — that is what makes the feature usable
    // as a posing tool and not only as an animation.
    EXPECT_FLOAT_EQ( AdvanceTimeOfDay( 7.5f, 1.0f, 0.0f ), 7.5f );
    EXPECT_FLOAT_EQ( AdvanceTimeOfDay( 7.5f, 1000.0f, 0.0f ), 7.5f );
}

// ---------------------------------------------------------------------------------------------------
// IBL rebake throttle
// ---------------------------------------------------------------------------------------------------

namespace
{
    // What Graphic::CloudEnvironmentFingerprint answers for a view with no cloud layer. The rebake rule
    // is written for the general case, so most of the tests below hold the cloud half still.
    constexpr uint64_t kNoClouds = 0ull;

    // ...and the same for the THIRD key, the sky's own parameters (Graphic::SkyBakeFingerprint). Held
    // still by every test in this section, which is about the sun and the clouds; the sky key has its own
    // tests in Desert/Tests/Engine/RendererSceneLifetime, where the defect that motivated it lives.
    //
    // Passed EXPLICITLY rather than defaulted in the rule's signature, and that is deliberate: a defaulted
    // staleness key reads as "no change" to every caller that forgets it, which is the failure the key was
    // added to fix wearing a compiler's approval.
    constexpr uint64_t kSameSky = 0ull;

    // A toward-sun direction @p degrees away from straight up, in the XY plane.
    glm::vec3 SunAt( float degrees )
    {
        const float r = glm::radians( degrees );
        return glm::vec3( std::sin( r ), std::cos( r ), 0.0f );
    }
} // namespace

TEST( Rebake, ExplicitRequestAlwaysBakes )
{
    for ( const bool autoRebake : { true, false } )
        for ( const bool hasEnv : { true, false } )
            EXPECT_TRUE( ShouldRebakeSkyEnvironment( SunAt( 0.0f ), SunAt( 0.0f ), 5.0f, autoRebake, hasEnv,
                                                     /*explicitRequest=*/true, kNoClouds, kNoClouds, kSameSky,
                                                     kSameSky ) );
}

TEST( Rebake, FirstBakeHappensEvenWithAutoRebakeOff )
{
    // Without this the scene has no ambient light at all. "Auto Rebake off" is a request to stop
    // RE-baking, not a request to render an unlit world.
    EXPECT_TRUE( ShouldRebakeSkyEnvironment( SunAt( 0.0f ), SunAt( 0.0f ), 5.0f, /*autoRebake=*/false,
                                             /*hasEnvironment=*/false, false, kNoClouds, kNoClouds, kSameSky,
                                             kSameSky ) );
}

TEST( RebakeDebounce, ADragCollapsesIntoOneBakeWhenItEnds )
{
    // The angular threshold says the environment is STALE; this rule says when to act on it. At 5 degrees
    // a drag crosses the threshold several times a second, and every crossing used to idle the device and
    // rebuild four cube images. Nothing runs while the sun is still moving...
    for ( float held = 0.0f; held < kSkyRebakeSettleSeconds; held += 0.01f )
        EXPECT_FALSE( SkyEnvironmentRebakeMayRun( held, /*secondsSinceStale=*/0.05f, kSkyRebakeSettleSeconds,
                                                  kSkyRebakeMaxDeferSeconds ) )
             << "sun still for " << held << " s";

    // ...and exactly once it stops.
    EXPECT_TRUE( SkyEnvironmentRebakeMayRun( kSkyRebakeSettleSeconds, 0.05f, kSkyRebakeSettleSeconds,
                                             kSkyRebakeMaxDeferSeconds ) );
}

TEST( RebakeDebounce, ASunThatNeverStopsStillGetsBaked )
{
    // The trap this bound exists for: the time-of-day driver moves the sun EVERY frame, so "wait until it
    // holds still" alone would defer the bake forever and freeze the environment at whatever hour the
    // scene was opened at. Sun permanently in motion, and it still refreshes.
    EXPECT_FALSE( SkyEnvironmentRebakeMayRun( 0.0f, kSkyRebakeMaxDeferSeconds * 0.5f, kSkyRebakeSettleSeconds,
                                              kSkyRebakeMaxDeferSeconds ) );
    EXPECT_TRUE( SkyEnvironmentRebakeMayRun( 0.0f, kSkyRebakeMaxDeferSeconds, kSkyRebakeSettleSeconds,
                                             kSkyRebakeMaxDeferSeconds ) );
}

TEST( RebakeDebounce, EitherConditionIsEnoughAndNeitherGoesBackwards )
{
    // Monotone in both arguments: waiting longer never turns a bake back off, which is what keeps the
    // deferral from oscillating at the boundary.
    for ( float still = 0.0f; still <= 0.30f; still += 0.01f )
    {
        bool seenTrue = false;
        for ( float stale = 0.0f; stale <= 2.0f; stale += 0.05f )
        {
            const bool may =
                 SkyEnvironmentRebakeMayRun( still, stale, kSkyRebakeSettleSeconds, kSkyRebakeMaxDeferSeconds );
            if ( seenTrue )
                EXPECT_TRUE( may ) << "still " << still << " stale " << stale;
            seenTrue = seenTrue || may;
        }
    }
}

TEST( Rebake, ThresholdIsHonouredOnBothSides )
{
    EXPECT_FALSE( ShouldRebakeSkyEnvironment( SunAt( 0.0f ), SunAt( 4.9f ), 5.0f, true, true, false, kNoClouds,
                                              kNoClouds, kSameSky, kSameSky ) );
    EXPECT_TRUE( ShouldRebakeSkyEnvironment( SunAt( 0.0f ), SunAt( 5.1f ), 5.0f, true, true, false, kNoClouds,
                                             kNoClouds, kSameSky, kSameSky ) );
}

TEST( Rebake, AutoRebakeOffSuppressesSunMovement )
{
    for ( const float move : { 1.0f, 45.0f, 179.0f } )
        EXPECT_FALSE( ShouldRebakeSkyEnvironment( SunAt( 0.0f ), SunAt( move ), 5.0f, /*autoRebake=*/false,
                                                  /*hasEnvironment=*/true, false, kNoClouds, kNoClouds, kSameSky,
                                                  kSameSky ) );
}

TEST( Rebake, AntipodalSunsDoNotProduceNaN )
{
    const glm::vec3 up   = glm::vec3( 0.0f, 1.0f, 0.0f );
    const glm::vec3 down = -up;

    // acos() of a dot product that lands on -1 - 1e-7 in float is NaN, and NaN compares false against
    // every threshold — which would silently disable rebaking forever rather than loudly break.
    EXPECT_TRUE( ShouldRebakeSkyEnvironment( up, down, 5.0f, true, true, false, kNoClouds, kNoClouds, kSameSky,
                                             kSameSky ) );
    EXPECT_TRUE( ShouldRebakeSkyEnvironment( up * 3.0f, down * 7.0f, 5.0f, true, true, false, kNoClouds, kNoClouds,
                                             kSameSky, kSameSky ) );
    EXPECT_FALSE(
         ShouldRebakeSkyEnvironment( up, up, 5.0f, true, true, false, kNoClouds, kNoClouds, kSameSky, kSameSky ) );
}

// ---------------------------------------------------------------------------------------------------
// Sky mode + duplicate skies
// ---------------------------------------------------------------------------------------------------

TEST( SkyModeRule, CoversAllFourCombinations )
{
    EXPECT_EQ( ResolveSkyMode( true, true ), SkyMode::Atmosphere ) << "the atmosphere wins the pass";
    EXPECT_EQ( ResolveSkyMode( true, false ), SkyMode::Atmosphere );
    EXPECT_EQ( ResolveSkyMode( false, true ), SkyMode::HdrCubemap );
    EXPECT_EQ( ResolveSkyMode( false, false ), SkyMode::None );
}

TEST( PrimarySky, PicksTheLowestIdAndIsOrderIndependent )
{
    const std::array<uint64_t, 3> a{ 90u, 12u, 55u };
    const std::array<uint64_t, 3> b{ 55u, 90u, 12u };

    ASSERT_TRUE( SelectPrimarySky( a ).has_value() );
    ASSERT_TRUE( SelectPrimarySky( b ).has_value() );
    EXPECT_EQ( a[*SelectPrimarySky( a )], 12u );
    EXPECT_EQ( b[*SelectPrimarySky( b )], 12u ) << "shuffling the scene must not change the sky";

    EXPECT_FALSE( SelectPrimarySky( std::span<const uint64_t>{} ).has_value() );
}

// ---------------------------------------------------------------------------------------------------
// Environment resolution and what it costs
// ---------------------------------------------------------------------------------------------------

TEST( EnvironmentResolution, LadderIsWorkGroupAligned )
{
    for ( const auto res :
          { SkyEnvironmentResolution::Low, SkyEnvironmentResolution::Medium, SkyEnvironmentResolution::High } )
    {
        const auto size = EnvironmentPanoramaSize( res );
        // The bake dispatches in 32x32 groups and the shader has no partial-group path, so a size that is
        // not a multiple of 32 leaves the panorama's right/bottom edge unwritten.
        EXPECT_EQ( size.Width % 32u, 0u );
        EXPECT_EQ( size.Height % 32u, 0u );
        EXPECT_EQ( size.Width, size.Height * 2u ) << "equirect panoramas are 2:1";
    }

    EXPECT_EQ( EnvironmentPanoramaSize( SkyEnvironmentResolution::Medium ).Width, 1024u )
         << "Medium must stay what the engine baked at unconditionally, or every scene changes look";
}

TEST( EnvironmentResolution, PanoramaCostIsTheAdvertisedNumber )
{
    // RGBA32F is 16 B/px: 2048*1024*16 = 32 MiB, 1024*512*16 = 8 MiB.
    EXPECT_NEAR( BytesToMiB( SkyEnvironmentBakeCost( SkyEnvironmentResolution::High ).PanoramaBytes ), 32.0,
                 1e-6 );
    EXPECT_NEAR( BytesToMiB( SkyEnvironmentBakeCost( SkyEnvironmentResolution::Medium ).PanoramaBytes ), 8.0,
                 1e-6 );
    EXPECT_NEAR( BytesToMiB( SkyEnvironmentBakeCost( SkyEnvironmentResolution::Low ).PanoramaBytes ), 2.0, 1e-6 );

    // The cube chain does NOT scale with the ladder — its faces are fixed constants either way. That is
    // the fact the log line exists to tell you, so it had better be true.
    const auto high = SkyEnvironmentBakeCost( SkyEnvironmentResolution::High );
    const auto low  = SkyEnvironmentBakeCost( SkyEnvironmentResolution::Low );
    EXPECT_EQ( high.CubeBytes, low.CubeBytes );
    EXPECT_EQ( high.TotalBytes, high.PanoramaBytes + high.CubeBytes );
}

// ---------------------------------------------------------------------------------------------------
// The IBL cube chain: the size that was asked for and the size the face gets are ONE quantity
// ---------------------------------------------------------------------------------------------------
//
// Three shipped defects were the same disagreement: a spec that carried the 4x3-cross extent while the
// image, the mip chain and the dispatch needed the FACE. A mip count derived from the cross was an
// invalid vkCreateImage (VUID-VkImageCreateInfo-mipLevels-00958 -> VK_ERROR_DEVICE_LOST); a dispatch
// derived from the cross launched 12x-16x the texel count; the prefiltered specular was created at a
// QUARTER of the face the cost report charged for. The spec now names the face, and these tests pin the
// relations each defect broke.

TEST( SkyEnvironmentCubes, MipChainLengthIsTheVulkanRule )
{
    // floor(log2(dim)) + 1, including non-powers-of-two and degenerate extents.
    EXPECT_EQ( MipChainLength( 1024u ), 11u );
    EXPECT_EQ( MipChainLength( 256u ), 9u );
    EXPECT_EQ( MipChainLength( 32u ), 6u );
    EXPECT_EQ( MipChainLength( 5u ), 3u );
    EXPECT_EQ( MipChainLength( 1u ), 1u );
    EXPECT_EQ( MipChainLength( 0u ), 1u );

    // The 2D helper is the same rule through the same function — a parallel log2 rounding its own way
    // is the "two implementations of one quantity" defect shape.
    EXPECT_EQ( Desert::Graphic::Utils::CalculateMipCount( 1024u, 512u ), MipChainLength( 1024u ) );
    EXPECT_EQ( Desert::Graphic::Utils::CalculateMipCount( 96u, 640u ), MipChainLength( 640u ) );
}

TEST( SkyEnvironmentCubes, EveryRequestedChainFitsItsOwnFace )
{
    // The (face, mips) pairs SceneEnvironment actually requests. A pair where mips exceeds the face's
    // chain is the VUID-00958 defect: VulkanImageCube now refuses it, so a violation here is a bake
    // that comes back with no environment at all.
    EXPECT_EQ( kSkyEnvPrefilterMips, MipChainLength( kSkyEnvPrefilterFaceSize ) )
         << "the prefilter walks the FULL chain of its face; a hand-typed count was how 11 mips got "
            "requested on a 256 face";
    EXPECT_LE( kSkyEnvRadianceMips, MipChainLength( kSkyEnvCubeFaceSize ) );
    EXPECT_GE( kSkyEnvRadianceMips, 1u );
}

TEST( SkyEnvironmentCubes, DispatchCoversEachFaceInWholeGroupsWithNoSurplusGroup )
{
    // The relation the 12x/16x dispatches broke: enough groups to cover every texel of the face, and
    // not one whole group more. (A cross-derived extent fails the second bound immediately.)
    const std::array<uint32_t, 5> faces = { kSkyEnvCubeFaceSize, kSkyEnvIrradianceFaceSize,
                                            kSkyEnvPrefilterFaceSize, 48u, 1u };
    for ( const uint32_t face : faces )
    {
        const uint32_t groups = DispatchGroupCount( face, kComputeImagesWorkGroupSize );
        EXPECT_GE( groups * kComputeImagesWorkGroupSize, face ) << "face " << face << " not covered";
        EXPECT_LT( ( groups - 1u ) * kComputeImagesWorkGroupSize, face )
             << "face " << face << " dispatches a surplus group row";
    }
}

TEST( SkyEnvironmentCubes, CostReportChargesForExactlyWhatTheBakeBuilds )
{
    // An independent mip walk over the SAME (face, mips) pairs SceneEnvironment passes to the compute
    // chain. The report once charged 1024/11 (128 MiB) for a prefiltered cube the bake built at 256/9
    // (8 MiB) — under a comment saying the two cannot disagree.
    const auto cubeBytes = []( uint32_t face, uint32_t mips )
    {
        uint64_t bytes = 0;
        for ( uint32_t mip = 0; mip < mips; ++mip )
        {
            const uint64_t side = std::max( 1u, face >> mip );
            bytes += 6ull * side * side * 16ull; // six faces, RGBA32F
        }
        return bytes;
    };

    const uint64_t expected = cubeBytes( kSkyEnvCubeFaceSize, kSkyEnvRadianceMips ) +
                              cubeBytes( kSkyEnvIrradianceFaceSize, 1u ) +
                              cubeBytes( kSkyEnvPrefilterFaceSize, kSkyEnvPrefilterMips );
    EXPECT_EQ( SkyEnvironmentBakeCost( SkyEnvironmentResolution::Medium ).CubeBytes, expected );
}

TEST( PlanetRadius, KilometresBecomeCentimetres )
{
    // 1 world unit = 1 cm, so the default 6360 km is 6.36e8 units.
    EXPECT_FLOAT_EQ( PlanetRadiusToWorldUnits( 6360.0f ), 636000000.0f );
    EXPECT_FLOAT_EQ( PlanetRadiusToWorldUnits( 1.0f ), Common::Units::Metres( 1000.0f ) );
}

// ---------------------------------------------------------------------------------------------------
// Component -> transport -> GPU payload
// ---------------------------------------------------------------------------------------------------

TEST( MakeSkySettingsRule, ConvertsTheArtistsUnitsIntoTheRenderers )
{
    SkyAtmosphereData data;
    data.SunAngularDiameter = 2.2918f; // degrees, DIAMETER
    data.PlanetRadius       = 6360.0f; // kilometres

    const SkySettings sky = MakeSkySettings( data );

    // Degrees -> radians AND diameter -> radius, both exactly once.
    EXPECT_NEAR( sky.SunAngularRadius, glm::radians( 2.2918f ) * 0.5f, 1e-7f );
    EXPECT_NEAR( glm::degrees( sky.SunAngularRadius ) * 2.0f, 2.2918f, 1e-4f );
    EXPECT_FLOAT_EQ( sky.PlanetRadius, 636000000.0f );

    // The palette and the bake knobs come across untouched.
    EXPECT_EQ( sky.ZenithColor, data.ZenithColor );
    EXPECT_EQ( sky.NightColor, data.NightColor );
    EXPECT_FLOAT_EQ( sky.StarIntensity, data.StarIntensity );
    EXPECT_EQ( sky.EnvironmentResolution, data.EnvironmentResolution );
    EXPECT_FLOAT_EQ( sky.RebakeSunAngleThreshold, data.RebakeSunAngleThreshold );
}

TEST( SkyPayloadLayout, EveryAuthoredValueLandsWhereTheShaderReadsIt )
{
    // The shader reads this block through Common/Atmosphere.glslh's unpack helpers, so the assertions
    // below ARE the shader's view of it: v[0].w is the sun intensity, v[6].w is the angular radius, and
    // so on. A member inserted in the middle fails here instead of corrupting the frame.
    // 13 lanes since the physical-atmosphere medium block (v[7]-v[12]) was APPENDED for the LUT passes;
    // 15 since Phase 2 appended the model switch and the art-direction tints (v[13]-v[14]).
    EXPECT_EQ( sizeof( SkyGpuPayload ), kSkyPackedVec4Count * sizeof( glm::vec4 ) );
    EXPECT_EQ( kSkyPayloadBytes, 15u * 16u );

    SkyAtmosphereData data;
    data.ZenithColor        = { 0.1f, 0.2f, 0.3f };
    data.HorizonColor       = { 0.4f, 0.5f, 0.6f };
    data.SunColor           = { 0.7f, 0.8f, 0.9f };
    data.SunsetColor        = { 0.11f, 0.12f, 0.13f };
    data.GroundColor        = { 0.14f, 0.15f, 0.16f };
    data.NightColor         = { 0.17f, 0.18f, 0.19f };
    data.SkyBrightness      = 1.25f;
    data.HorizonFalloff     = 0.65f;
    data.SunGlow            = 2.5f;
    data.SunsetIntensity    = 1.75f;
    data.StarIntensity      = 3.25f;
    data.SunIntensity       = 17.0f;
    data.SunAngularDiameter = 4.0f;

    data.SkyLuminanceFactor                     = { 0.21f, 0.22f, 0.23f };
    data.SkyAndAerialPerspectiveLuminanceFactor = { 0.24f, 0.25f, 0.26f };

    const glm::vec3     toward = glm::normalize( glm::vec3( 0.3f, 0.9f, 0.3f ) );
    const SkyGpuPayload p      = PackSky( toward, MakeSkySettings( data ) );

    const auto* lanes = reinterpret_cast<const glm::vec4*>( &p );

    EXPECT_EQ( glm::vec3( lanes[0] ), toward );
    EXPECT_FLOAT_EQ( lanes[0].w, 17.0f );

    EXPECT_EQ( glm::vec3( lanes[1] ), data.ZenithColor );
    EXPECT_FLOAT_EQ( lanes[1].w, 1.25f );
    EXPECT_EQ( glm::vec3( lanes[2] ), data.HorizonColor );
    EXPECT_FLOAT_EQ( lanes[2].w, 0.65f );
    EXPECT_EQ( glm::vec3( lanes[3] ), data.SunColor );
    EXPECT_FLOAT_EQ( lanes[3].w, 2.5f );
    EXPECT_EQ( glm::vec3( lanes[4] ), data.SunsetColor );
    EXPECT_FLOAT_EQ( lanes[4].w, 1.75f );
    EXPECT_EQ( glm::vec3( lanes[5] ), data.GroundColor );
    EXPECT_FLOAT_EQ( lanes[5].w, 3.25f );
    EXPECT_EQ( glm::vec3( lanes[6] ), data.NightColor );
    EXPECT_NEAR( lanes[6].w, glm::radians( 4.0f ) * 0.5f, 1e-7f ) << "RADIANS, and a radius";

    // The medium block, exactly where SkyMedium.glslh's SkyMakeAtmParams expects each lane. The
    // coefficients arrive as MakeSkySettings' scale x colour PRODUCTS (per kilometre), and the planet
    // radius arrives in WORLD UNITS — the km conversion belongs to the shader, and only to it.
    EXPECT_EQ( glm::vec3( lanes[7] ),
               data.RayleighScatteringScale * data.RayleighScattering ); // 0.0331 x colour, /km
    EXPECT_FLOAT_EQ( lanes[7].w, data.RayleighExponentialDistribution );
    EXPECT_EQ( glm::vec3( lanes[8] ), data.MieScatteringScale * data.MieScattering );
    EXPECT_FLOAT_EQ( lanes[8].w, data.MieExponentialDistribution );
    EXPECT_EQ( glm::vec3( lanes[9] ), data.MieAbsorptionScale * data.MieAbsorption );
    EXPECT_FLOAT_EQ( lanes[9].w, data.MieAnisotropy );
    EXPECT_EQ( glm::vec3( lanes[10] ), data.OtherAbsorptionScale * data.OtherAbsorption );
    EXPECT_FLOAT_EQ( lanes[10].w, data.AtmosphereHeight );
    EXPECT_EQ( glm::vec3( lanes[11] ), data.GroundAlbedo );
    EXPECT_FLOAT_EQ( lanes[11].w, data.MultiScatteringFactor );
    EXPECT_FLOAT_EQ( lanes[12].x, data.AbsorptionTipAltitude );
    EXPECT_FLOAT_EQ( lanes[12].y, data.AbsorptionTipValue );
    EXPECT_FLOAT_EQ( lanes[12].z, data.AbsorptionTentWidth );
    EXPECT_FLOAT_EQ( lanes[12].w, PlanetRadiusToWorldUnits( data.PlanetRadius ) );

    // The Phase 2 lanes: the art-direction tints, and the model switch packed as EXACTLY 0 or 1 so
    // the shader's `> 0.5` branch is never a float hazard.
    EXPECT_EQ( glm::vec3( lanes[13] ), data.SkyLuminanceFactor );
    EXPECT_FLOAT_EQ( lanes[13].w, 0.0f ) << "ArtisticGradient packs 0";
    EXPECT_EQ( glm::vec3( lanes[14] ), data.SkyAndAerialPerspectiveLuminanceFactor );
    EXPECT_FLOAT_EQ( lanes[14].w, 0.0f ) << "reserved lane";

    data.Model                        = Desert::ECS::SkyModel::PhysicalAtmosphere;
    const SkyGpuPayload physical      = PackSky( toward, MakeSkySettings( data ) );
    const auto*         physicalLanes = reinterpret_cast<const glm::vec4*>( &physical );
    EXPECT_FLOAT_EQ( physicalLanes[13].w, 1.0f ) << "PhysicalAtmosphere packs 1";
}

// ---------------------------------------------------------------------------------------------------
// The evaluated state the other renderers consume
// ---------------------------------------------------------------------------------------------------

TEST( AtmosphereEnvRule, AmbientIsTheDomeNotTheZenithTexel )
{
    // The sky ambient is the hemisphere a surface sits under, blended toward the horizon colour by solid
    // angle — the zenith texel alone is the dome's darkest, bluest corner, and feeding it alone painted
    // every shadowed face navy. The ground term is what the ground REFLECTS (sun + dome, times albedo
    // over pi), not the tone the ground is painted with.
    SkySettings sky;
    sky.ZenithColor   = { 0.08f, 0.26f, 0.70f };
    sky.HorizonColor  = { 0.50f, 0.66f, 0.92f };
    sky.NightColor    = { 0.01f, 0.02f, 0.05f };
    sky.GroundColor   = { 0.16f, 0.19f, 0.24f };
    sky.SkyBrightness = 2.0f;

    const auto day = EvaluateAtmosphere( sky, glm::vec3( 0.0f, 1.0f, 0.0f ) );

    const glm::vec3 dome = glm::mix( sky.ZenithColor, sky.HorizonColor, 0.65f ) * sky.SkyBrightness;
    EXPECT_NEAR( day.ZenithRadiance.r, dome.r, 1e-5f ) << "dome blend, scaled by Sky Brightness";
    EXPECT_NEAR( day.ZenithRadiance.b, dome.b, 1e-5f );
    EXPECT_GT( day.ZenithRadiance.r / day.ZenithRadiance.b, 0.08f / 0.70f )
         << "the dome must be less blue than the zenith texel alone";

    const glm::vec3 expectedGround =
         sky.GroundColor * ( day.SunIrradiance * 1.0f + day.ZenithRadiance ) * 0.3183099f;
    EXPECT_NEAR( day.GroundRadiance.r, expectedGround.r, 1e-4f )
         << "ground bounce reflects sun + dome, Lambertian";
    EXPECT_NEAR( day.GroundRadiance.b, expectedGround.b, 1e-4f );

    const auto night = EvaluateAtmosphere( sky, glm::vec3( 0.0f, -1.0f, 0.0f ) );
    EXPECT_NEAR( night.ZenithRadiance.b, 0.05f * 2.0f, 1e-5f ) << "night: the dome resolves to the night colour";

    const glm::vec3 expectedNightGround = 0.30f * sky.GroundColor * night.ZenithRadiance * 0.3183099f;
    EXPECT_NEAR( night.GroundRadiance.b, expectedNightGround.b, 1e-5f )
         << "at night the ground reflects only the night sky";
}

TEST( AtmosphereEnvRule, TheSunTakesTheSunsetColourAsItGoesDown )
{
    // SunIrradiance is the SKY's own sun, and the only route by which the colour of the light reaches the
    // consumers that read it. It used to be SunColor * SunIntensity with no dependence on elevation, so
    // the sky reddened at dusk and everything lit by this irradiance stayed noon-white. The tint now
    // mirrors Atmosphere.glslh's own — mix(sunsetColor, sunColor, smoothstep(0, 0.25, sunUp)) — which is
    // what makes "every consumer sees one sun" a fact rather than a comment.
    SkyAtmosphereData data;
    data.SunColor     = { 1.0f, 1.0f, 1.0f }; // deliberately neutral: any warmth must come from the ramp
    data.SunsetColor  = { 1.0f, 0.4f, 0.2f };
    data.SunIntensity = 10.0f;

    const SkySettings sky = MakeSkySettings( data );

    // High: the sun's own colour, untouched.
    const auto high = EvaluateAtmosphere( sky, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    EXPECT_NEAR( high.SunIrradiance.r / high.SunIrradiance.b, 1.0f, 1e-5f );

    // On the horizon: the sunset colour, and therefore red-dominant.
    const auto low = EvaluateAtmosphere( sky, glm::vec3( 1.0f, 0.0f, 0.0f ) );
    EXPECT_GT( low.SunIrradiance.r / low.SunIrradiance.b, 4.0f );

    // And it reddens MONOTONICALLY on the way down — no band that jumps.
    float previousRatio = 0.0f;
    for ( float y = 0.30f; y >= 0.0f; y -= 0.02f )
    {
        const auto  env   = EvaluateAtmosphere( sky, glm::vec3( 1.0f, y, 0.0f ) );
        const float ratio = env.SunIrradiance.r / std::max( env.SunIrradiance.b, 1e-6f );
        EXPECT_GE( ratio, previousRatio - 1e-5f ) << "sun y = " << y;
        previousRatio = ratio;
    }
}

TEST( AtmosphereEnvRule, TheSunStopsLightingThingsOnceItIsDown )
{
    // The irradiance follows the DISC: once the sun is genuinely below the horizon it contributes nothing,
    // and what lights the world after sunset is the night sky, which arrives through ZenithRadiance
    // instead. Before this ramp existed a scene at midnight went on receiving the full noon irradiance
    // while the sky behind it had gone dark.
    SkyAtmosphereData data;
    data.SunColor     = { 1.0f, 1.0f, 1.0f };
    data.SunIntensity = 10.0f;

    const SkySettings sky   = MakeSkySettings( data );
    const auto        night = EvaluateAtmosphere( sky, glm::vec3( 0.0f, -1.0f, 0.0f ) );

    EXPECT_NEAR( night.SunIrradiance.r, 0.0f, 1e-6f );
    EXPECT_NEAR( night.SunIrradiance.g, 0.0f, 1e-6f );
    EXPECT_NEAR( night.SunIrradiance.b, 0.0f, 1e-6f );

    // Never negative on the way there, and never brighter than the daylight value.
    const auto noon = EvaluateAtmosphere( sky, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    for ( float y = 1.0f; y >= -1.0f; y -= 0.05f )
    {
        const auto env = EvaluateAtmosphere( sky, glm::vec3( 0.3f, y, 0.0f ) );
        EXPECT_GE( env.SunIrradiance.r, 0.0f ) << "sun y = " << y;
        EXPECT_LE( env.SunIrradiance.r, noon.SunIrradiance.r + 1e-5f ) << "sun y = " << y;
    }
}

TEST( AtmosphereEnvRule, CarriesTheSunAndNormalizes )
{
    SkyAtmosphereData data;
    data.SunColor           = { 1.0f, 0.5f, 0.25f };
    data.SunIntensity       = 4.0f;
    data.SunAngularDiameter = 2.2918f;
    data.PlanetRadius       = 6360.0f;

    const auto env = EvaluateAtmosphere( MakeSkySettings( data ),
                                         glm::vec3( 0.0f, 5.0f, 0.0f ) /*deliberately un-normalized*/ );

    EXPECT_NEAR( glm::length( env.SunDirection ), 1.0f, 1e-6f );
    EXPECT_FLOAT_EQ( env.SunIrradiance.r, 4.0f );
    EXPECT_FLOAT_EQ( env.SunIrradiance.g, 2.0f );
    EXPECT_TRUE( env.Valid );
}

TEST( AtmosphereEnvRule, DefaultConstructedStateIsInvalid )
{
    // What the renderer publishes when no enabled sky drives the frame. A consumer that drew anyway would
    // be drawing against the previous frame's sun.
    const Desert::Graphic::AtmosphereEnv env;
    EXPECT_FALSE( env.Valid );
}

TEST( ColorTemperature, MatchesUnrealsConversionAndBehavesPhysically )
{
    using Desert::Graphic::ColorFromTemperature;

    // 6500 K is the D65-adjacent illuminant: every channel lands near 1 (within ten percent) without
    // being exactly white — the property that matters, pinned instead of a secondhand sample value.
    // The formula itself is transcribed verbatim from FLinearColor::MakeFromColorTemperature (Krystek
    // 1985 -> xyY -> XYZ -> linear BT.709), constants and all.
    const glm::vec3 d65 = ColorFromTemperature( 6500.0f );
    EXPECT_NEAR( d65.r, 1.0f, 0.1f );
    EXPECT_NEAR( d65.g, 1.0f, 0.1f );
    EXPECT_NEAR( d65.b, 1.0f, 0.1f );
    EXPECT_GT( d65.r, d65.g ) << "slightly warm of pure white, as the locus is at 6500 K";

    // A candle is red-dominant, a clear-sky blue is blue-dominant, and the red:blue ratio falls
    // MONOTONICALLY with temperature — the property a hue slider is trusted for.
    EXPECT_GT( ColorFromTemperature( 1800.0f ).r, ColorFromTemperature( 1800.0f ).b * 3.0f );
    EXPECT_GT( ColorFromTemperature( 12000.0f ).b, ColorFromTemperature( 12000.0f ).r );

    float previous = 1e9f;
    for ( float k = 1000.0f; k <= 15000.0f; k += 250.0f )
    {
        const glm::vec3 c     = ColorFromTemperature( k );
        const float     ratio = c.r / glm::max( c.b, 1e-4f );
        EXPECT_LE( ratio, previous + 1e-4f ) << "kelvin = " << k;
        previous = ratio;
    }

    // The conversion clamps to its published domain rather than extrapolating the fit.
    EXPECT_EQ( ColorFromTemperature( 100.0f ), ColorFromTemperature( 1000.0f ) );
    EXPECT_EQ( ColorFromTemperature( 50000.0f ), ColorFromTemperature( 15000.0f ) );
}

// ---------------------------------------------------------------------------------------------------
// The clouds' half of the rebake trigger
// ---------------------------------------------------------------------------------------------------
//
// Since Р15 the panorama the IBL chain descends from is marched with the cloud layer in it
// (Programs/Compute/BakeProceduralSky.shader), so "the environment is stale" stopped being a question
// about the sun alone. What follows tests the RELATION rather than any number: which changes to the
// field the fingerprint must see, and — the harder half — which it must NOT, because a bake idles the
// device for three quarters of a second and two of the block's fields move every frame of every scene
// that has a breeze in it or a camera that walks.

namespace
{
    // A sky that is ordinary rather than interesting: what is measured below is which CHANGES move the
    // fingerprint, so the baseline only has to be a valid packed block.
    Desert::Graphic::AtmosphereEnv CloudTestAtmosphere()
    {
        Desert::Graphic::AtmosphereEnv env;
        env.Valid                  = true;
        env.SunDirection           = glm::normalize( glm::vec3( 0.3f, 0.8f, 0.2f ) );
        env.SunIrradiance          = glm::vec3( 8.0f, 7.4f, 6.6f );
        env.SunIlluminanceOnGround = glm::vec3( 8.0f, 7.4f, 6.6f );
        env.ZenithRadiance         = glm::vec3( 0.20f, 0.28f, 0.44f );
        env.GroundRadiance         = glm::vec3( 0.10f, 0.10f, 0.09f );
        return env;
    }

    CloudTypeShape CloudTestShape()
    {
        // VALUE-INITIALISED, and the brace is load-bearing: CloudTypeShape is an aggregate with no
        // default member initialisers, so `CloudTypeShape shape;` leaves the anvil fields indeterminate —
        // and CloudTypeSetEnvelopeKm reads them, which puts stack garbage into the layer's envelope and
        // therefore into the fingerprint. The first draft of this suite did exactly that and reported the
        // wind exclusion as broken.
        CloudTypeShape shape{};
        shape.BaseAltitudeKm   = 1.5f;
        shape.TopAltitudeKm    = 3.5f;
        shape.DetailCharacter  = 0.5f;
        shape.DetailFactor     = 1.0f;
        shape.DensityFactor    = 1.0f;
        shape.ExtinctionFactor = 1.0f;
        return shape;
    }

    // @p wind and @p regionOrigin are the two things the fingerprint must ignore, so they are arguments
    // rather than constants — a helper that could not vary them could not state the property.
    uint64_t CloudTestFingerprint( const Desert::ECS::VolumetricCloudData& data, const glm::vec3& wind,
                                   const glm::vec2& regionOrigin, bool skyOcclusionValid = false,
                                   uint32_t                                    shapeGeneration = 0u,
                                   const Desert::Graphic::CloudMaterialValues& material        = {},
                                   uint64_t mediumVariant = 0ull, uint64_t mediumValues = 0ull )
    {
        const CloudTypeShape shape      = CloudTestShape();
        const auto           atmosphere = CloudTestAtmosphere();

        const CloudGpuPayload payload = PackCloudParams( data, material, &shape, 1u, atmosphere, wind,
                                                         CloudRegionBinding{ regionOrigin, 30.0f } );
        return CloudEnvironmentFingerprint( payload, /*marched=*/true, skyOcclusionValid, shapeGeneration,
                                            mediumVariant, mediumValues );
    }
} // namespace

TEST( CloudEnvironmentCadence, NoCloudsIsZeroAndCloudsNeverAre )
{
    // The two states have to be tellable apart, or deleting the layer would leave its overcast baked into
    // the scene's ambient with nothing to trigger a rebake.
    const CloudGpuPayload empty{};
    EXPECT_EQ( CloudEnvironmentFingerprint( empty, /*marched=*/false, false, 0u, 0ull, 0ull ), 0ull );
    EXPECT_EQ( CloudEnvironmentFingerprint( empty, /*marched=*/false, true, 7u, 0xABCDull, 0xBEEFull ), 0ull );

    Desert::ECS::VolumetricCloudData data;
    EXPECT_NE( CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ) ), 0ull );
}

TEST( CloudEnvironmentCadence, TheAuthoredMediumIsVisibleAndItIsInNOTHINGELSEHERE )
{
    // THE ONE INPUT THAT IS CODE. Everything else the panorama shows arrives in the packed block, so
    // hashing the block covers it; a cloud material's authored MEDIUM is a body of GPU code compiled
    // into the march, and it changes what the panorama shows while leaving every byte of that block
    // identical. Left out of the fingerprint, authoring a medium would move the sky on screen and leave
    // the light in the world coming from the previous one — indefinitely, because a bake is only
    // triggered by this number. That is the "middle link drops a property" shape in the one link that
    // costs three quarters of a second to re-run.
    Desert::ECS::VolumetricCloudData     data;
    Desert::Graphic::CloudMaterialValues material;

    const uint64_t shipped = CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), false, 0u, material,
                                                   /*mediumVariant=*/0ull );
    const uint64_t authored = CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), false, 0u,
                                                    material, /*mediumVariant=*/0x51A7ull );
    const uint64_t other = CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), false, 0u, material,
                                                 /*mediumVariant=*/0x51A8ull );

    EXPECT_NE( shipped, authored ) << "authoring a cloud medium left the environment fingerprint where it "
                                      "was, so the panorama the scene is LIT by would keep showing the "
                                      "medium before it.";
    EXPECT_NE( authored, other ) << "two different authored media share one fingerprint";
    EXPECT_EQ( shipped,
               CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), false, 0u, material, 0ull ) )
         << "the fingerprint is a function of the medium's content, not of how many have been seen";
}

TEST( CloudEnvironmentCadence, TheAuthoredMediumsOWNVALUESAreVisibleAndAreASeparateAxisFromItsCode )
{
    // О1-G-2, and it is the SAME defect one link further along. A medium's code being in the fingerprint
    // is not enough once that code reads values a `.demat` supplies: the same graph with a different tint
    // is a different sky, and every byte of the packed block is identical for both, because those values
    // belong to a schema the graph author wrote and travel in a buffer of their own.
    Desert::ECS::VolumetricCloudData     data;
    Desert::Graphic::CloudMaterialValues material;

    const auto fp = [&]( uint64_t code, uint64_t values ) {
        return CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), false, 0u, material, code,
                                     values );
    };

    EXPECT_NE( fp( 0x51A7ull, 0ull ), fp( 0x51A7ull, 0x1234ull ) )
         << "authoring a medium's own parameter left the environment fingerprint where it was, so the "
            "panorama the scene is LIT by would keep showing the value before it.";
    EXPECT_NE( fp( 0x51A7ull, 0x1234ull ), fp( 0x51A7ull, 0x1235ull ) )
         << "two different sets of medium values share one fingerprint";

    // AND THEY ARE TWO INPUTS, NOT ONE. Folded together by the caller — an XOR, which is the obvious
    // thing to reach for — two equal hashes would cancel to zero and a medium change would be invisible
    // exactly when its code and its values happened to hash alike. The function takes both and mixes
    // them in turn, so no pair can annihilate.
    EXPECT_NE( fp( 0xABCDull, 0xABCDull ), fp( 0ull, 0ull ) )
         << "the medium's code and its values cancelled each other out; they are not one input.";
    EXPECT_NE( fp( 0xABCDull, 0ull ), fp( 0ull, 0xABCDull ) )
         << "swapping the medium's code hash and its value hash gave the same answer, so the two axes are "
            "not distinguishable.";
}

TEST( CloudEnvironmentCadence, WindAndTheRegionOriginAreDeliberatelyInvisible )
{
    // THIS IS THE DECISION THE WHOLE FUNCTION EXISTS FOR. The diffuse irradiance cube is a 65 536-sample
    // cosine convolution per texel: it integrates the ARRANGEMENT of the field away and responds only to
    // the dome's mean, so advection moves nothing it can see. And the modelling region's origin follows
    // the CAMERA. A fingerprint that saw either would idle the device for three quarters of a second
    // every time the wind blew or a player walked three kilometres — the same feature, shipped as a
    // stutter.
    Desert::ECS::VolumetricCloudData data;

    const uint64_t base = CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ) );

    for ( const float km : { 0.5f, 40.0f, 5000.0f } )
    {
        EXPECT_EQ( CloudTestFingerprint( data, glm::vec3( km, 0.0f, -km ) * 100000.0f, glm::vec2( 0.0f ) ), base )
             << "wind " << km << " km";
        EXPECT_EQ( CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( km, -km ) ), base )
             << "region origin " << km << " km";
    }
}

TEST( CloudEnvironmentCadence, EveryMaterialKnobTheMarchReadsIsSeen )
{
    // The half of the trigger that lives in the packed block: what the cloud is MADE OF and how it is lit.
    // Taken through the WHOLE block rather than field by field, so a parameter appended to
    // CloudGpuPayload tomorrow is in the fingerprint the moment it exists.
    // THE KNOBS ARE THE MATERIAL'S SINCE O1 — the fingerprint has to see a `.demat` edit exactly as it
    // saw the component fields, because the material editor's live drag is now how these values move.
    Desert::ECS::VolumetricCloudData           data;
    const Desert::Graphic::CloudMaterialValues stock{};
    const uint64_t base = CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ) );

    const auto moved = [&]( const Desert::Graphic::CloudMaterialValues& changed, const char* what )
    {
        EXPECT_NE( CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), false, 0u, changed ), base )
             << what;
    };

    Desert::Graphic::CloudMaterialValues density = stock;
    density.DensityScale                         = stock.DensityScale * 1.5f + 0.1f;
    moved( density, "Density Scale" );

    Desert::Graphic::CloudMaterialValues extinction = stock;
    extinction.ExtinctionScale                      = stock.ExtinctionScale * 1.5f + 0.1f;
    moved( extinction, "Extinction Scale" );

    Desert::Graphic::CloudMaterialValues albedo = stock;
    albedo.ScatteringAlbedo                     = stock.ScatteringAlbedo * 0.5f;
    moved( albedo, "Scattering Albedo" );

    Desert::Graphic::CloudMaterialValues detail = stock;
    detail.DetailStrength                       = stock.DetailStrength * 0.5f + 0.05f;
    moved( detail, "Detail Strength" );

    Desert::Graphic::CloudMaterialValues phase = stock;
    phase.PhaseG                               = stock.PhaseG * 0.5f;
    moved( phase, "Phase G" );
}

TEST( CloudEnvironmentCadence, WhereTheCloudIsArrivesThroughTheShapeGeneration )
{
    // Coverage, the cloud types, the seed, the placement lattice and the painted layout decide WHERE cloud
    // is, and NONE of them is in the packed block at all — they are consumed on the CPU by
    // Assets::BakeCloudProceduralVolume and reach the march only as the contents of the modelling volume.
    // A fingerprint over the block alone would therefore miss the most important knob on the panel, which
    // is exactly the dead-trigger shape DEV_CONTRACT §1.3 is about.
    Desert::ECS::VolumetricCloudData data;

    const uint64_t base = CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), false, 0u );

    // Every distinct rebuild of the volume's SHAPE is a distinct environment, and they do not collide with
    // each other.
    std::array<uint64_t, 4> seen{ base, 0ull, 0ull, 0ull };
    for ( uint32_t generation = 1; generation <= 3; ++generation )
    {
        seen[generation] = CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), false, generation );
        for ( uint32_t earlier = 0; earlier < generation; ++earlier )
            EXPECT_NE( seen[generation], seen[earlier] ) << generation << " vs " << earlier;
    }
}

TEST( CloudEnvironmentCadence, TheSkyOcclusionVolumeIsPartOfIt )
{
    // The volume is written by a pass INSIDE the frame and the bake runs before it, so the first bake of
    // a scene necessarily marches without it and the dome comes out brighter than the visible sky by the
    // whole of the layer's self-occlusion. Carrying the flag in the fingerprint is what makes the frame it
    // appears ask for one more bake instead of leaving that difference standing for the session.
    Desert::ECS::VolumetricCloudData data;

    EXPECT_NE( CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), /*skyOcclusion=*/false ),
               CloudTestFingerprint( data, glm::vec3( 0.0f ), glm::vec2( 0.0f ), /*skyOcclusion=*/true ) );
}

TEST( Rebake, CloudsMovingRebakeAStillSun )
{
    // The two halves of the trigger are independent, and this is the half that did not exist before: an
    // artist dragging Coverage while the sun stands still has to see the ambient follow.
    EXPECT_TRUE( ShouldRebakeSkyEnvironment( SunAt( 0.0f ), SunAt( 0.0f ), 5.0f, /*autoRebake=*/true,
                                             /*hasEnvironment=*/true, /*explicitRequest=*/false, 0x1234ull,
                                             0x5678ull, kSameSky, kSameSky ) );

    // ...and an unchanged sky under an unchanged sun still bakes nothing.
    EXPECT_FALSE( ShouldRebakeSkyEnvironment( SunAt( 0.0f ), SunAt( 0.0f ), 5.0f, true, true, false, 0x1234ull,
                                              0x1234ull, kSameSky, kSameSky ) );

    // Auto Rebake off suppresses the cloud half exactly as it suppresses the sun's, because it means "stop
    // following the sky", not "stop following one part of it".
    EXPECT_FALSE( ShouldRebakeSkyEnvironment( SunAt( 0.0f ), SunAt( 0.0f ), 5.0f, /*autoRebake=*/false, true,
                                              false, 0x1234ull, 0x5678ull, kSameSky, kSameSky ) );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ONE BAKE WHEN THE INPUTS ARE READY. Startup used to bake the sky alone, then with clouds, then with the
// sky-occlusion volume: three bakes where the last had every answer. The rule waits while the layer's
// inputs are pending and never while they are complete or absent.
TEST( SkyEnvironmentBakeWaitsForClouds, WaitsOnlyWhileTheCloudInputsArePending )
{
    EXPECT_TRUE( SkyEnvironmentBakeWaitsForClouds( false, true ) );
    EXPECT_FALSE( SkyEnvironmentBakeWaitsForClouds( false, false ) );
}

TEST( SkyEnvironmentBakeWaitsForClouds, TheBakeButtonNeverWaits )
{
    EXPECT_FALSE( SkyEnvironmentBakeWaitsForClouds( true, true ) );
    EXPECT_FALSE( SkyEnvironmentBakeWaitsForClouds( true, false ) );
}

// THE STARTUP SEQUENCE, replayed through the rule and the rebake trigger together: pending frames bake
// nothing, the first frame with every input bakes exactly once, and later identical frames bake nothing.
TEST( SkyEnvironmentBakeWaitsForClouds, StartupBakesExactlyOnceWhenInputsLand )
{
    const glm::vec3 sun( 0.0f, 1.0f, 0.0f );
    bool            hasEnvironment = false;
    uint64_t        bakedCloud     = 0;
    uint64_t        bakedSky       = 0;
    int             bakes          = 0;

    struct Frame
    {
        bool     Pending;
        uint64_t Cloud;
    };
    // Noise on a worker, modelling bake in flight, sky occlusion undecided, then ready for good.
    const Frame frames[] = { { true, 0 }, { true, 0 }, { true, 11 }, { false, 12 }, { false, 12 }, { false, 12 } };
    for ( const Frame& f : frames )
    {
        if ( SkyEnvironmentBakeWaitsForClouds( false, f.Pending ) )
            continue;
        if ( !ShouldRebakeSkyEnvironment( sun, sun, 5.0f, true, hasEnvironment, false, bakedCloud, f.Cloud,
                                          bakedSky, 7 ) )
            continue;
        ++bakes;
        hasEnvironment = true;
        bakedCloud     = f.Cloud;
        bakedSky       = 7;
    }
    EXPECT_EQ( bakes, 1 );
    EXPECT_EQ( bakedCloud, 12u );
}
