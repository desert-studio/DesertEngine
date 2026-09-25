// Cascaded-shadow-map fitting, tested as numbers.
//
// This exists because of a bug that shipped and could not have been caught by anything we had: the
// cascade coverage was a bare `150.0f` written when a world unit was a METRE. The switch to centimetres
// left the number alone, so shadows were computed for the first metre and a half in front of the camera
// and nothing beyond it was ever shadowed. Every test below is about a distance in world units; several
// of them fail outright against the old constant.

#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

#include <Common/Core/Units.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <cmath>

using Desert::Graphic::ApplyShadowQuality;
using Desert::Graphic::CascadeFit;
using Desert::Graphic::CascadeSetup;
using Desert::Graphic::ComputeShadowCascades;
using Desert::Graphic::kMaxShadowCascades;

namespace Units = Common::Units;

namespace
{
    // A camera looking down -Z from the origin with the engine's own defaults (near 10 cm, far 50 km).
    //
    // THE PROJECTION MUST COME FROM THE ENGINE'S OWN FACTORY. ComputeShadowCascades unprojects NDC
    // corners, so it reads the camera's depth convention; a glm::perspective here would hand it an
    // OpenGL [-1,1] matrix, the near and far rings would swap, and the test would happily pin cascades
    // fitted behind the observer.
    CascadeSetup DefaultSetup()
    {
        CascadeSetup s;
        s.CameraNear = Desert::Core::kDefaultNearPlane;
        s.CameraFar  = Desert::Core::kDefaultFarPlane;
        s.CameraView = glm::lookAt( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0, 1, 0 ) );
        s.CameraProjection =
             Desert::Core::MakePerspective( glm::radians( 45.0f ), 16.0f / 9.0f, s.CameraNear, s.CameraFar );
        s.LightDirection   = glm::normalize( glm::vec3( -0.4f, -1.0f, -0.3f ) ); // where the light TRAVELS
        return s;
    }

    // Is @p worldPos inside cascade @p c's shadow map (i.e. would it receive a shadow)?
    bool CoveredBy( const CascadeFit& fit, const glm::vec3& worldPos )
    {
        const glm::vec4 clip = fit.ViewProj * glm::vec4( worldPos, 1.0f );
        const glm::vec3 ndc  = glm::vec3( clip ) / clip.w;
        return ndc.x >= -1.0f && ndc.x <= 1.0f && ndc.y >= -1.0f && ndc.y <= 1.0f && ndc.z >= 0.0f &&
               ndc.z <= 1.0f;
    }
} // namespace

// THE regression test. A character-sized object 40 m down the view axis is ordinary scene content; with
// the metre-era constant the cascades stopped at 1.5 m and nothing here was covered.
TEST( ShadowCascades, CoversOrdinaryWorldDistances )
{
    const CascadeSetup setup = DefaultSetup();
    CascadeFit         fits[kMaxShadowCascades];
    const uint32_t     n = ComputeShadowCascades( setup, fits );
    ASSERT_EQ( n, kMaxShadowCascades );

    for ( const float metres : { 1.0f, 5.0f, 20.0f, 40.0f, 100.0f } )
    {
        const glm::vec3 p( 0.0f, 0.0f, -Units::Metres( metres ) );
        bool            covered = false;
        for ( uint32_t c = 0; c < n; ++c )
            covered = covered || CoveredBy( fits[c], p );
        EXPECT_TRUE( covered ) << "nothing shadows a point " << metres << " m in front of the camera";
    }
}

// The coverage cap is the number that decides whether a scene has shadows at all, so it is stated in
// metres and asserted in metres.
TEST( ShadowCascades, LastCascadeReachesTheRequestedDistance )
{
    CascadeSetup setup = DefaultSetup();
    setup.MaxDistance  = Units::Metres( 150.0f );

    CascadeFit     fits[kMaxShadowCascades];
    const uint32_t n = ComputeShadowCascades( setup, fits );
    ASSERT_EQ( n, kMaxShadowCascades );

    EXPECT_NEAR( fits[n - 1].SplitFar, setup.MaxDistance, setup.MaxDistance * 0.01f );
    EXPECT_GT( fits[n - 1].SplitFar, Units::Metres( 100.0f ) );
}

// A cascade that is not capped follows the CAMERA's far plane instead, so a short-range camera does not
// get cascades stretched over a kilometre of nothing.
TEST( ShadowCascades, CoverageIsCappedByTheNearerOfFarPlaneAndMaxDistance )
{
    CascadeSetup setup = DefaultSetup();
    setup.CameraFar    = Units::Metres( 30.0f );
    setup.CameraProjection =
         Desert::Core::MakePerspective( glm::radians( 45.0f ), 16.0f / 9.0f, setup.CameraNear, setup.CameraFar );
    setup.MaxDistance = Units::Metres( 150.0f );

    CascadeFit     fits[kMaxShadowCascades];
    const uint32_t n = ComputeShadowCascades( setup, fits );
    ASSERT_EQ( n, kMaxShadowCascades );
    EXPECT_NEAR( fits[n - 1].SplitFar, Units::Metres( 30.0f ), Units::Metres( 0.5f ) );
}

TEST( ShadowCascades, SplitsAndRadiiGrowOutward )
{
    CascadeFit     fits[kMaxShadowCascades];
    const uint32_t n = ComputeShadowCascades( DefaultSetup(), fits );
    ASSERT_EQ( n, kMaxShadowCascades );

    for ( uint32_t c = 1; c < n; ++c )
    {
        EXPECT_GT( fits[c].SplitFar, fits[c - 1].SplitFar ) << "cascade " << c << " must reach further";
        EXPECT_GE( fits[c].Radius, fits[c - 1].Radius ) << "a further slice cannot bound smaller";
    }
    EXPECT_GT( fits[0].Radius, 0.0f );
}

// The normal-offset bias in the shader is expressed in these units; if the relation breaks, shadow acne
// scales wrongly and the failure looks like "shadows are noisy" rather than anything about texels.
TEST( ShadowCascades, WorldPerTexelMatchesRadiusAndMapSize )
{
    CascadeSetup setup  = DefaultSetup();
    setup.ShadowMapSize = 2048;

    CascadeFit     fits[kMaxShadowCascades];
    const uint32_t n = ComputeShadowCascades( setup, fits );
    ASSERT_EQ( n, kMaxShadowCascades );

    for ( uint32_t c = 0; c < n; ++c )
        EXPECT_FLOAT_EQ( fits[c].WorldPerTexel, ( 2.0f * fits[c].Radius ) / 2048.0f );
}

// The texel snap is what stops shadows crawling as the camera moves: the cascade centre may only travel
// in WHOLE shadow-map texels along the light's own axes, so the sampling grid stays world-locked. It does
// NOT mean the centre never moves — it follows the camera along the view axis, and the light-space depth
// axis is deliberately not snapped. So the assertion is about the light-space X/Y being on the grid.
TEST( ShadowCascades, CentreMovesInWholeTexelsAlongTheLightAxes )
{
    CascadeSetup setup = DefaultSetup();
    CascadeFit   before[kMaxShadowCascades];
    ASSERT_EQ( ComputeShadowCascades( setup, before ), kMaxShadowCascades );

    const glm::vec3 lightDir   = glm::normalize( setup.LightDirection );
    const glm::vec3 up         = glm::abs( lightDir.y ) > 0.99f ? glm::vec3( 0, 0, 1 ) : glm::vec3( 0, 1, 0 );
    const glm::mat4 lightBasis = glm::lookAt( -lightDir, glm::vec3( 0.0f ), up );

    // Creep the camera sideways in tenths of a texel and check every intermediate position.
    for ( int step = 1; step <= 12; ++step )
    {
        const float nudge = before[0].WorldPerTexel * 0.1f * static_cast<float>( step );
        setup.CameraView =
             glm::lookAt( glm::vec3( nudge, 0.0f, 0.0f ), glm::vec3( nudge, 0.0f, -1.0f ), glm::vec3( 0, 1, 0 ) );

        CascadeFit after[kMaxShadowCascades];
        ASSERT_EQ( ComputeShadowCascades( setup, after ), kMaxShadowCascades );

        const glm::vec3 a = glm::vec3( lightBasis * glm::vec4( before[0].Center, 1.0f ) );
        const glm::vec3 b = glm::vec3( lightBasis * glm::vec4( after[0].Center, 1.0f ) );

        for ( int axis = 0; axis < 2; ++axis ) // x, y — the two axes the snap owns
        {
            const float texels = ( b[axis] - a[axis] ) / before[0].WorldPerTexel;
            EXPECT_NEAR( texels, std::round( texels ), 1e-2f )
                 << "cascade slid " << texels << " texels on axis " << axis << " at step " << step;
        }
    }
}

// ── The shadow BUDGET ─────────────────────────────────────────────────────────────────────────────
//
// A preview renderer allocates one cascade at 1024 instead of four at 2048, and the temptation is to
// assert the three numbers. That would pin the decision without pinning what it is FOR: the numbers move
// together (see ShadowQuality's own note), and one cascade at 1024 over the SCENE's 150 m is a legal set
// of numbers that produces a blob. What matters is the world size of one texel against the thing being
// shadowed, and that is what these assert.

namespace
{
    // The preview camera as PreviewViewport drives it: a 35-degree lens looking at a 1 m primitive from
    // about 1.8 m, which is what its own fit computes for a sphere.
    CascadeSetup PreviewSetup()
    {
        CascadeSetup s;
        s.CameraNear = Units::Cm( 1.0f );
        s.CameraFar  = Units::Metres( 1000.0f );
        s.CameraView =
             glm::lookAt( glm::vec3( 0.0f, 0.6f, 1.8f ) * 100.0f, glm::vec3( 0.0f ), glm::vec3( 0, 1, 0 ) );
        s.CameraProjection =
             Desert::Core::MakePerspective( glm::radians( 35.0f ), 1.0f, s.CameraNear, s.CameraFar );
        s.LightDirection = glm::normalize( glm::vec3( 2.0f, -6.0f, 5.0f ) );
        return s;
    }
} // namespace

// The two DRAWING budgets are not interchangeable and the difference is the whole point of the type.
// (The third, kNoShadowQuality, draws nothing and is asserted separately below.)
TEST( ShadowQualityBudget, ThePresetsDifferInAllThreeNumbers )
{
    using Desert::Graphic::kPreviewShadowQuality;
    using Desert::Graphic::kSceneShadowQuality;

    EXPECT_EQ( kSceneShadowQuality.CascadeCount, kMaxShadowCascades );
    EXPECT_EQ( kSceneShadowQuality.ShadowMapSize, 2048u );
    EXPECT_FLOAT_EQ( kSceneShadowQuality.MaxDistance, Units::Metres( 150.0f ) );

    EXPECT_LT( kPreviewShadowQuality.CascadeCount, kSceneShadowQuality.CascadeCount );
    EXPECT_LT( kPreviewShadowQuality.ShadowMapSize, kSceneShadowQuality.ShadowMapSize );
    EXPECT_LT( kPreviewShadowQuality.MaxDistance, kSceneShadowQuality.MaxDistance );
}

// WHY THE BUDGET EXISTS, as a number. Attachment bytes are cascades x size^2 x (R32F + D24S8); the
// preview must be at least an order cheaper or it is not worth having a second budget at all.
//
// Through Graphic::ShadowAttachmentBytes, not a lambda. This test used to carry its own copy of the
// formula beside the renderer's, which is the mirror-with-no-guard shape: two spellings of one quantity,
// and the test could only ever agree with itself.
TEST( ShadowQualityBudget, ThePreviewCostsAnOrderOfMagnitudeLess )
{
    using Desert::Graphic::kPreviewShadowQuality;
    using Desert::Graphic::kSceneShadowQuality;
    using Desert::Graphic::ShadowAttachmentBytes;

    EXPECT_EQ( ShadowAttachmentBytes( kSceneShadowQuality ), 134217728ull ); // 128 MiB
    EXPECT_EQ( ShadowAttachmentBytes( kPreviewShadowQuality ), 8388608ull );   // 8 MiB
    EXPECT_LE( ShadowAttachmentBytes( kPreviewShadowQuality ) * 10ull,
               ShadowAttachmentBytes( kSceneShadowQuality ) );
}

// ── A renderer without shadows holds no shadow attachments ────────────────────────────────────────
//
// THE RELATION, not the number: whatever the third preset's fields say, what must hold is that a budget
// which draws no cascade also COSTS no cascade, and that the two are the same fact rather than two facts
// that happen to agree today. Asserting `kNoShadowQuality.CascadeCount == 0` alone would pass just as
// well if ShadowAttachmentBytes had a floor in it, or if the size field were still 2048 and some later
// allocation read the size without the count.
TEST( ShadowQualityBudget, ABudgetThatDrawsNoCascadeCostsNoBytes )
{
    using Desert::Graphic::ShadowAttachmentBytes;
    using Desert::Graphic::ShadowQuality;

    // Both directions of the iff, over the shipped presets and over the half-states a careless edit
    // produces: a count of zero with a full-size map still standing, and a full count at size zero.
    const ShadowQuality cases[] = { Desert::Graphic::kNoShadowQuality, Desert::Graphic::kPreviewShadowQuality,
                                    Desert::Graphic::kSceneShadowQuality,
                                    ShadowQuality{ 0u, 2048u, Units::Metres( 150.0f ) },
                                    ShadowQuality{ 4u, 0u, Units::Metres( 150.0f ) } };

    for ( const auto& q : cases )
    {
        const bool drawsNothing = q.CascadeCount == 0 || q.ShadowMapSize == 0;
        EXPECT_EQ( ShadowAttachmentBytes( q ) == 0ull, drawsNothing )
             << "count " << q.CascadeCount << ", size " << q.ShadowMapSize;
    }

    // And the shipped preset is on the free side of it.
    EXPECT_EQ( ShadowAttachmentBytes( Desert::Graphic::kNoShadowQuality ), 0ull );
}

// The fitter agrees with the budget: a renderer that allocated nothing must also be told to write no
// cascade matrices. This is the same middle-link failure ApplyShadowQuality exists to prevent, seen from
// the zero end — a fitter that produced four matrices for a renderer holding no maps would have the
// shader walk cascades that were never rendered.
TEST( ShadowQualityBudget, ANoShadowBudgetFitsNoCascades )
{
    CascadeSetup setup = DefaultSetup();
    ApplyShadowQuality( setup, Desert::Graphic::kNoShadowQuality );

    CascadeFit fits[kMaxShadowCascades];
    EXPECT_EQ( ComputeShadowCascades( setup, fits ), 0u );
}

// THE LIVE TOTAL the renderer's log line prints. It exists because two "320 MiB" lines in one editor log
// were read as 640 MiB held at once, when they were one renderer allocating, releasing and allocating
// again. What that reading needed and did not have is how many sets are ALIVE — so the property under
// test is that the total tracks construction and destruction exactly, and that a shadowless renderer
// contributes nothing to it and does not count as a holder.
TEST( ShadowQualityBudget, TheLiveTotalTracksWhatIsHeldAndNotWhatWasEverAllocated )
{
    using Desert::Graphic::ShadowAttachmentBytes;
    using Desert::Graphic::ShadowAttachmentLease;

    const uint64_t base      = ShadowAttachmentLease::LiveBytes();
    const uint32_t baseHold  = ShadowAttachmentLease::LiveHolders();
    const uint64_t sceneCost = ShadowAttachmentBytes( Desert::Graphic::kSceneShadowQuality );

    {
        const ShadowAttachmentLease shadowless{ Desert::Graphic::kNoShadowQuality };
        EXPECT_EQ( ShadowAttachmentLease::LiveBytes(), base );
        EXPECT_EQ( ShadowAttachmentLease::LiveHolders(), baseHold )
             << "a renderer holding no cascades must not be counted as one that does";
    }

    {
        const ShadowAttachmentLease first{ Desert::Graphic::kSceneShadowQuality };
        EXPECT_EQ( ShadowAttachmentLease::LiveBytes(), base + sceneCost );
        EXPECT_EQ( ShadowAttachmentLease::LiveHolders(), baseHold + 1u );
    }

    // Released, exactly as SceneRenderer::Init drops the old MeshRenderer before building the new one.
    EXPECT_EQ( ShadowAttachmentLease::LiveBytes(), base );
    EXPECT_EQ( ShadowAttachmentLease::LiveHolders(), baseHold );

    {
        // The empty editor's second allocation. TWO log lines, ONE live set — the whole point.
        const ShadowAttachmentLease second{ Desert::Graphic::kSceneShadowQuality };
        EXPECT_EQ( ShadowAttachmentLease::LiveBytes(), base + sceneCost );
        EXPECT_EQ( ShadowAttachmentLease::LiveHolders(), baseHold + 1u );

        // And two genuinely concurrent renderers do add up, or the total would be useless in the
        // direction it is actually meant to warn about: six live windows.
        const ShadowAttachmentLease alsoLive{ Desert::Graphic::kPreviewShadowQuality };
        EXPECT_EQ( ShadowAttachmentLease::LiveBytes(),
                   base + sceneCost + ShadowAttachmentBytes( Desert::Graphic::kPreviewShadowQuality ) );
        EXPECT_EQ( ShadowAttachmentLease::LiveHolders(), baseHold + 2u );
    }

    EXPECT_EQ( ShadowAttachmentLease::LiveBytes(), base );
    EXPECT_EQ( ShadowAttachmentLease::LiveHolders(), baseHold );
}

// THE RELATION, and the reason the distance is part of the budget rather than left at the scene's. One
// texel of the preview's single cascade must be small against the thing it shadows — the primitives are
// 100 world units (1 m) across, so a texel worth more than a fiftieth of that is a blob with an outline.
// Against the SCENE's 150 m the same one cascade at 1024 fails this by a wide margin, which is exactly
// the mistake the three-numbers-together note warns about.
TEST( ShadowQualityBudget, OnePreviewCascadeResolvesAPreviewSizedObject )
{
    constexpr float kPrimitiveSize = 100.0f; // world units, PrimitiveMeshFactory::kPrimitiveSize

    CascadeSetup setup = PreviewSetup();
    ApplyShadowQuality( setup, Desert::Graphic::kPreviewShadowQuality );

    CascadeFit     fits[kMaxShadowCascades];
    const uint32_t n = ComputeShadowCascades( setup, fits );
    ASSERT_EQ( n, 1u ) << "the preview budget is one cascade; the fitter must not produce more";
    EXPECT_LE( fits[0].WorldPerTexel, kPrimitiveSize / 50.0f )
         << "one texel is " << fits[0].WorldPerTexel << " world units against a " << kPrimitiveSize
         << "-unit subject";

    // The same single cascade over the SCENE's distance — the tempting half-change — does not.
    CascadeSetup tooFar = PreviewSetup();
    ApplyShadowQuality( tooFar, Desert::Graphic::ShadowQuality{ 1u, 1024u, Units::Metres( 150.0f ) } );
    CascadeFit farFits[kMaxShadowCascades];
    ASSERT_EQ( ComputeShadowCascades( tooFar, farFits ), 1u );
    EXPECT_GT( farFits[0].WorldPerTexel, kPrimitiveSize / 50.0f )
         << "if this ever passes, the distance has stopped mattering and the budget can lose a field";
}

// ApplyShadowQuality is the one assignment site precisely so a link cannot drop one of the three.
TEST( ShadowQualityBudget, ApplyCarriesAllThreeNumbers )
{
    CascadeSetup                         setup;
    const Desert::Graphic::ShadowQuality q{ 2u, 512u, Units::Metres( 37.0f ) };
    ApplyShadowQuality( setup, q );

    EXPECT_EQ( setup.CascadeCount, q.CascadeCount );
    EXPECT_EQ( setup.ShadowMapSize, q.ShadowMapSize );
    EXPECT_FLOAT_EQ( setup.MaxDistance, q.MaxDistance );
}

TEST( ShadowCascades, DegenerateSetupsProduceNothing )
{
    CascadeFit fits[kMaxShadowCascades];

    CascadeSetup noLight   = DefaultSetup();
    noLight.LightDirection = glm::vec3( 0.0f );
    EXPECT_EQ( ComputeShadowCascades( noLight, fits ), 0u );

    CascadeSetup inverted = DefaultSetup();
    inverted.MaxDistance  = inverted.CameraNear * 0.5f;
    EXPECT_EQ( ComputeShadowCascades( inverted, fits ), 0u );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
