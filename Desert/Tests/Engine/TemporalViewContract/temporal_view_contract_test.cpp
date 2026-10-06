// TEMPORAL VIEW CONTRACT (TAA1-C0): the per-view temporal foundation — the resolution split, the temporal method
// of a frame, the jitter, SceneViewState's frame protocol and its history resets, MotionHistory, and the
// RDG-FAULT1 policies of velocity and history — pinned BEFORE the implementation exists. Written against the
// headers only; it does not link until TAA1's implementation step lands, and that step is done when this suite
// passes unchanged. No GPU: the graph is a bare RDG::Builder that is never executed, and the upscaler is a fixture
// that declares the same history TAA does.
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGFault.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <Engine/Core/Projection.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <set>
#include <vector>

using namespace Desert::Graphic;
namespace Scal = Common::Scalability;

namespace
{
    constexpr ViewExtent k1080p{ 1920, 1080 };

    // ResultStr::GetValue is deleted on an rvalue (it would hand out a reference into a dying temporary).
    template <typename T>
    T Value( const Common::ResultStr<T>& result )
    {
        EXPECT_TRUE( result.IsSuccess() ) << ( result.IsSuccess() ? std::string() : result.GetError() );
        return result.GetValue();
    }

    Scal::PathAntiAliasing Path( Scal::AntiAliasingMethod method )
    {
        Scal::PathAntiAliasing path;
        path.Method      = method;
        path.PostProcess = ( method == Scal::AntiAliasingMethod::FXAA || method == Scal::AntiAliasingMethod::SMAA )
                                ? method
                                : Scal::AntiAliasingMethod::None;
        return path;
    }

    ResolutionSplit Split( ViewExtent output, int percent )
    {
        const auto split = MakeResolutionSplit( output, percent );
        EXPECT_TRUE( split.IsSuccess() );
        return split.GetValue();
    }

    // Declares exactly what the engine's TAA declares: one RGBA16F colour history at the split's output side.
    class FixtureUpscaler final : public ITemporalUpscaler
    {
    public:
        explicit FixtureUpscaler( TemporalMethod method ) : m_Method( method )
        {
        }

        std::string_view DebugName() const override
        {
            return "Fixture";
        }
        TemporalMethod Method() const override
        {
            return m_Method;
        }
        bool Supports( const ResolutionSplit& split ) const override
        {
            return ( m_Method == TemporalMethod::TAAU ) == ( split.Mode == Scal::ScaleMode::Upscale );
        }
        std::vector<HistoryTextureDesc> HistoryDescs( const ResolutionSplit& split ) const override
        {
            const ViewExtent   side = split.Mode == Scal::ScaleMode::Supersample ? split.Render : split.Output;
            HistoryTextureDesc desc;
            desc.Desc.Size   = { side.Width, side.Height, 1 };
            desc.Desc.Format = Desert::Core::Formats::ImageFormat::RGBA16F;
            desc.Name        = "TAA.History";
            return { desc };
        }
        Common::ResultStr<TemporalUpscalerOutputs> AddPasses( RDG::Builder&, const ViewFrame&,
                                                              const TemporalUpscalerInputs& inputs ) const override
        {
            return Common::MakeSuccess( TemporalUpscalerOutputs{ inputs.History.front().Current } );
        }

    private:
        TemporalMethod m_Method;
    };

    ViewInputs Inputs( double time, glm::vec3 eye = { 0.0f, 150.0f, 0.0f } )
    {
        ViewInputs in;
        in.View       = glm::lookAt( eye, eye + glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        in.Projection = Desert::Core::MakePerspective( glm::radians( 60.0f ), 16.0f / 9.0f, 10.0f, 5'000'000.0f );
        in.CameraPosition = eye;
        in.NearPlane      = 10.0f;
        in.FarPlane       = 5'000'000.0f;
        in.CameraIdentity = 7;
        in.SceneIdentity  = 1;
        in.Output         = k1080p;
        in.AntiAliasing   = Path( Scal::AntiAliasingMethod::TAA );
        in.TimeSeconds    = time;
        return in;
    }

    glm::vec2 ToNdc( const glm::mat4& viewProj, const glm::vec3& world )
    {
        const glm::vec4 clip = viewProj * glm::vec4( world, 1.0f );
        return glm::vec2( clip ) / clip.w;
    }

    bool Near( const glm::mat4& a, const glm::mat4& b, float eps = 1e-6f )
    {
        for ( int c = 0; c < 4; ++c )
            for ( int r = 0; r < 4; ++r )
                if ( std::abs( a[c][r] - b[c][r] ) > eps * std::max( 1.0f, std::abs( a[c][r] ) ) )
                    return false;
        return true;
    }
} // namespace

// ---- Resolution split ---------------------------------------------------------------------------------------

TEST( TemporalViewContract, SplitRoundsToNearestAndClassifiesLikeScalability )
{
    const ResolutionSplit quality = Split( k1080p, 67 ); // 1286.4 x 723.6
    EXPECT_EQ( quality.Render, ( ViewExtent{ 1286, 724 } ) );
    EXPECT_EQ( quality.Output, k1080p );
    EXPECT_EQ( quality.Mode, Scal::ScaleMode::Upscale );

    const ResolutionSplit native = Split( k1080p, 100 );
    EXPECT_EQ( native.Render, native.Output );
    EXPECT_EQ( native.Mode, Scal::ScaleMode::Native );

    const ResolutionSplit ssaa = Split( k1080p, 150 );
    EXPECT_EQ( ssaa.Render, ( ViewExtent{ 2880, 1620 } ) );
    EXPECT_EQ( ssaa.Mode, Scal::ScaleMode::Supersample );
}

TEST( TemporalViewContract, SplitNeverRendersBelowOnePixelAndRefusesAnUnallocatableSide )
{
    EXPECT_EQ( Split( ViewExtent{ 1, 1 }, 50 ).Render, ( ViewExtent{ 1, 1 } ) );
    // 200 % of a side above kMaxViewExtentSide / 2: an error naming the numbers, never a silently lowered percent.
    const auto huge = MakeResolutionSplit( ViewExtent{ kMaxViewExtentSide / 2 + 1, 1080 }, 200 );
    ASSERT_FALSE( huge.IsSuccess() );
    EXPECT_NE( huge.GetError().find( "200" ), std::string::npos );
}

// ---- Temporal method ----------------------------------------------------------------------------------------

TEST( TemporalViewContract, MethodFollowsTheTwoAxes )
{
    using M = Scal::AntiAliasingMethod;
    EXPECT_EQ( Value( SelectTemporalMethod( Path( M::TAA ), Split( k1080p, 100 ), Scal::Upscaler::None ) ),
               TemporalMethod::TAA );
    EXPECT_EQ( Value( SelectTemporalMethod( Path( M::TAA ), Split( k1080p, 150 ), Scal::Upscaler::None ) ),
               TemporalMethod::TAA );
    EXPECT_EQ( Value( SelectTemporalMethod( Path( M::FXAA ), Split( k1080p, 100 ), Scal::Upscaler::None ) ),
               TemporalMethod::None );
    EXPECT_EQ( Value( SelectTemporalMethod( Path( M::SMAA ), Split( k1080p, 150 ), Scal::Upscaler::None ) ),
               TemporalMethod::None );
    EXPECT_EQ( Value( SelectTemporalMethod( Path( M::TAA ), Split( k1080p, 67 ), Scal::Upscaler::TAAU ) ),
               TemporalMethod::TAAU );
}

TEST( TemporalViewContract, MethodRefusesWhatScalabilityNeverProduces )
{
    using M = Scal::AntiAliasingMethod;
    // Below 100 % with no upscaler, and an upscaler at native scale: SCAL1's Resolve rule 5 removes both.
    EXPECT_FALSE( SelectTemporalMethod( Path( M::TAA ), Split( k1080p, 67 ), Scal::Upscaler::None ).IsSuccess() );
    EXPECT_FALSE( SelectTemporalMethod( Path( M::TAA ), Split( k1080p, 100 ), Scal::Upscaler::TAAU ).IsSuccess() );
    // A vendor upscaler has no implementation in this build: an error naming it, not a quiet TAAU.
    for ( const Scal::Upscaler vendor :
          { Scal::Upscaler::FSR, Scal::Upscaler::DLSS, Scal::Upscaler::XeSS, Scal::Upscaler::MetalFX } )
    {
        const auto chosen = SelectTemporalMethod( Path( M::TAA ), Split( k1080p, 67 ), vendor );
        EXPECT_FALSE( chosen.IsSuccess() );
    }
}

// ---- Jitter -------------------------------------------------------------------------------------------------

TEST( TemporalViewContract, JitterLengthIsEightPerOutputPixel )
{
    EXPECT_EQ( TemporalJitterSequenceLength( TemporalMethod::None, Split( k1080p, 100 ) ), 0u );
    EXPECT_EQ( TemporalJitterSequenceLength( TemporalMethod::TAA, Split( k1080p, 100 ) ), 8u );
    EXPECT_EQ( TemporalJitterSequenceLength( TemporalMethod::TAA, Split( k1080p, 150 ) ), 8u );
    EXPECT_EQ( TemporalJitterSequenceLength( TemporalMethod::TAAU, Split( k1080p, 50 ) ), 32u ); // 8 x 4
    // 8 x (1920*1080)/(1286*724) = 17.82 -> 18
    EXPECT_EQ( TemporalJitterSequenceLength( TemporalMethod::TAAU, Split( k1080p, 67 ) ), 18u );
    ResolutionSplit quarter = Split( k1080p, 50 );
    quarter.Render          = { 480, 270 };
    EXPECT_EQ( TemporalJitterSequenceLength( TemporalMethod::TAAU, quarter ), 64u ); // 128 capped
}

TEST( TemporalViewContract, JitterIsHalton23CentredAndWraps )
{
    // index 0 -> Halton(1): (1/2, 1/3) - 0.5
    const glm::vec2 first = TemporalJitterPixels( 0, 8 );
    EXPECT_FLOAT_EQ( first.x, 0.0f );
    EXPECT_FLOAT_EQ( first.y, 1.0f / 3.0f - 0.5f );
    // index 1 -> Halton(2): (1/4, 2/3) - 0.5
    const glm::vec2 second = TemporalJitterPixels( 1, 8 );
    EXPECT_FLOAT_EQ( second.x, -0.25f );
    EXPECT_FLOAT_EQ( second.y, 2.0f / 3.0f - 0.5f );

    std::set<std::pair<float, float>> distinct;
    for ( uint32_t i = 0; i < 8; ++i )
    {
        const glm::vec2 j = TemporalJitterPixels( i, 8 );
        EXPECT_GE( j.x, -0.5f );
        EXPECT_LT( j.x, 0.5f );
        EXPECT_GE( j.y, -0.5f );
        EXPECT_LT( j.y, 0.5f );
        distinct.insert( { j.x, j.y } );
        EXPECT_EQ( TemporalJitterPixels( i + 8, 8 ), j );
    }
    EXPECT_EQ( distinct.size(), 8u );
    EXPECT_EQ( TemporalJitterPixels( 5, 0 ), glm::vec2( 0.0f ) );
}

TEST( TemporalViewContract, JitterShiftsTheWholeImageByExactlyItsNdcForPerspectiveAndOrtho )
{
    EXPECT_EQ( JitterPixelsToNdc( { 0.5f, -0.25f }, ViewExtent{ 1000, 500 } ), glm::vec2( 0.001f, -0.001f ) );
    EXPECT_EQ( JitterPixelsToNdc( { 0.5f, 0.5f }, ViewExtent{} ), glm::vec2( 0.0f ) );

    const glm::vec2 ndc( 0.0013f, -0.0021f );
    const glm::mat4 view = glm::lookAt( glm::vec3( 0, 0, 0 ), glm::vec3( 0, 0, -1 ), glm::vec3( 0, 1, 0 ) );
    for ( const glm::mat4& proj :
          { Desert::Core::MakePerspective( glm::radians( 60.0f ), 1.5f, 10.0f, 5'000'000.0f ),
            Desert::Core::MakeOrthographic( -500.0f, 500.0f, -300.0f, 300.0f, 1.0f, 10'000.0f ) } )
    {
        const glm::mat4 jittered = ApplyJitter( proj, ndc );
        for ( const glm::vec3 p :
              { glm::vec3( 0, 0, -50 ), glm::vec3( 120, -40, -900 ), glm::vec3( -7, 3, -4000 ) } )
        {
            const glm::vec2 delta = ToNdc( jittered * view, p ) - ToNdc( proj * view, p );
            EXPECT_NEAR( delta.x, ndc.x, 1e-6f );
            EXPECT_NEAR( delta.y, ndc.y, 1e-6f );
        }
    }
}

// ---- SceneViewState: the frame protocol ---------------------------------------------------------------------

TEST( TemporalViewContract, FirstFrameReprojectsOntoItself )
{
    SceneViewState        state;
    const FixtureUpscaler taa( TemporalMethod::TAA );
    const auto            frame = state.BeginFrame( Inputs( 1.0 ), &taa );
    ASSERT_TRUE( frame.IsSuccess() );
    const ViewFrame& f = frame.GetValue();
    EXPECT_EQ( f.HistoryReset, HistoryResetReason::FirstFrame );
    EXPECT_FALSE( f.HistoryValid() );
    EXPECT_EQ( f.PrevViewProjection, f.ViewProjection );
    EXPECT_EQ( f.PrevCameraPosition, f.CameraPosition );
    EXPECT_EQ( f.DeltaSeconds, 0.0f );
    EXPECT_EQ( f.JitterIndex, 0u );
    EXPECT_EQ( f.Method, TemporalMethod::TAA );
    EXPECT_EQ( f.JitterSequenceLength, 8u );
}

TEST( TemporalViewContract, CommittedFrameBecomesPreviousAndJitterAdvances )
{
    SceneViewState        state;
    const FixtureUpscaler taa( TemporalMethod::TAA );
    const ViewFrame       first = Value( state.BeginFrame( Inputs( 1.0 ), &taa ) );
    state.EndFrame( RDG::ExecuteReport{} );

    const ViewFrame second = Value( state.BeginFrame( Inputs( 1.25, { 30.0f, 150.0f, 0.0f } ), &taa ) );
    EXPECT_EQ( second.HistoryReset, HistoryResetReason::None );
    EXPECT_TRUE( second.HistoryValid() );
    EXPECT_EQ( second.PrevView, first.View );
    EXPECT_EQ( second.PrevViewProjection, first.ViewProjection );
    EXPECT_EQ( second.PrevJitteredViewProjection, first.JitteredViewProjection );
    EXPECT_EQ( second.PrevJitterNdc, first.JitterNdc );
    EXPECT_EQ( second.PrevCameraPosition, first.CameraPosition );
    EXPECT_DOUBLE_EQ( second.PrevTimeSeconds, 1.0 );
    EXPECT_FLOAT_EQ( second.DeltaSeconds, 0.25f );
    EXPECT_EQ( second.JitterIndex, 1u );
    EXPECT_EQ( second.FrameIndex, first.FrameIndex + 1 );
}

TEST( TemporalViewContract, MatricesFollowTheJitterRule )
{
    SceneViewState        state;
    const FixtureUpscaler taa( TemporalMethod::TAA );
    const ViewFrame       f = Value( state.BeginFrame( Inputs( 1.0 ), &taa ) );
    EXPECT_EQ( f.JitterNdc, JitterPixelsToNdc( f.JitterPixels, f.Split.Render ) );
    EXPECT_TRUE( Near( f.JitteredProjection, ApplyJitter( f.Projection, f.JitterNdc ) ) );
    EXPECT_TRUE( Near( f.ViewProjection, f.Projection * f.View ) );
    EXPECT_TRUE( Near( f.JitteredViewProjection, f.JitteredProjection * f.View ) );
    EXPECT_TRUE( Near( f.InvJitteredViewProjection * f.JitteredViewProjection, glm::mat4( 1.0f ), 1e-4f ) );
    EXPECT_EQ( f.MaterialMipBias, 0.0f );
}

TEST( TemporalViewContract, NonTemporalFrameIsUnjitteredAndHoldsNoHistory )
{
    SceneViewState state;
    ViewInputs     in = Inputs( 1.0 );
    in.AntiAliasing   = Path( Scal::AntiAliasingMethod::FXAA );
    const ViewFrame f = Value( state.BeginFrame( in, nullptr ) );
    EXPECT_EQ( f.Method, TemporalMethod::None );
    EXPECT_EQ( f.JitterSequenceLength, 0u );
    EXPECT_EQ( f.JitterNdc, glm::vec2( 0.0f ) );
    EXPECT_EQ( f.JitteredProjection, f.Projection ); // bit-identical to a pre-TAA1 frame
    EXPECT_EQ( state.History().HeldBytes(), 0u );
}

TEST( TemporalViewContract, UpscaleBiasesMaterialMipsByTheScale )
{
    SceneViewState        state;
    const FixtureUpscaler taau( TemporalMethod::TAAU );
    ViewInputs            in = Inputs( 1.0 );
    in.RenderScalePercent    = 50;
    in.Upscaler              = Scal::Upscaler::TAAU;
    const ViewFrame f        = Value( state.BeginFrame( in, &taau ) );
    EXPECT_EQ( f.Method, TemporalMethod::TAAU );
    EXPECT_EQ( f.Split.Render, ( ViewExtent{ 960, 540 } ) );
    EXPECT_FLOAT_EQ( f.MaterialMipBias, -1.0f );
}

TEST( TemporalViewContract, UpscalerThatDoesNotMatchTheMethodIsRefused )
{
    SceneViewState        state;
    const FixtureUpscaler taau( TemporalMethod::TAAU );
    EXPECT_FALSE( state.BeginFrame( Inputs( 1.0 ), &taau ).IsSuccess() );   // TAA frame, TAAU object
    EXPECT_FALSE( state.BeginFrame( Inputs( 1.0 ), nullptr ).IsSuccess() ); // TAA frame, no object
}

TEST( TemporalViewContract, EveryResetReasonResetsThePreviousFrame )
{
    const FixtureUpscaler taa( TemporalMethod::TAA );
    const auto            expectReset = [&]( auto mutate, HistoryResetReason reason )
    {
        SceneViewState state;
        (void)state.BeginFrame( Inputs( 1.0 ), &taa );
        state.EndFrame( RDG::ExecuteReport{} );
        ViewInputs next = Inputs( 1.25, { 500.0f, 150.0f, 0.0f } );
        mutate( next );
        const auto frame = state.BeginFrame( next, next.AntiAliasing.Method == Scal::AntiAliasingMethod::TAA
                                                        ? static_cast<const ITemporalUpscaler*>( &taa )
                                                        : nullptr );
        ASSERT_TRUE( frame.IsSuccess() );
        const ViewFrame& f = frame.GetValue();
        EXPECT_EQ( f.HistoryReset, reason );
        EXPECT_EQ( f.PrevViewProjection, f.ViewProjection );
        EXPECT_EQ( f.DeltaSeconds, 0.0f );
        EXPECT_EQ( f.JitterIndex, 0u );
    };
    expectReset( []( ViewInputs& in ) { in.CameraCut = true; }, HistoryResetReason::CameraCut );
    expectReset( []( ViewInputs& in ) { in.CameraIdentity = 8; }, HistoryResetReason::CameraCut );
    expectReset( []( ViewInputs& in ) { in.Output = { 1280, 720 }; }, HistoryResetReason::Resize );
    expectReset( []( ViewInputs& in ) { in.RenderScalePercent = 150; }, HistoryResetReason::Resize );
    expectReset( []( ViewInputs& in ) { in.AntiAliasing = Path( Scal::AntiAliasingMethod::SMAA ); },
                 HistoryResetReason::TemporalMethodChange );
}

TEST( TemporalViewContract, AQualityChangeKeepsTheHistory )
{
    SceneViewState        state;
    const FixtureUpscaler taa( TemporalMethod::TAA );
    (void)state.BeginFrame( Inputs( 1.0 ), &taa );
    state.EndFrame( RDG::ExecuteReport{} );
    ViewInputs next = Inputs( 1.25 );
    next.Quality    = TemporalAAQuality::High;
    EXPECT_EQ( Value( state.BeginFrame( next, &taa ) ).HistoryReset, HistoryResetReason::None );
}

TEST( TemporalViewContract, AFrameThatNeverEndedIsAsIfItNeverHappened )
{
    SceneViewState        state;
    const FixtureUpscaler taa( TemporalMethod::TAA );
    const ViewFrame       committed = Value( state.BeginFrame( Inputs( 1.0 ), &taa ) );
    state.EndFrame( RDG::ExecuteReport{} );
    (void)state.BeginFrame( Inputs( 1.25, { 80.0f, 150.0f, 0.0f } ), &taa ); // FrameFault: no EndFrame
    const ViewFrame after = Value( state.BeginFrame( Inputs( 1.5, { 90.0f, 150.0f, 0.0f } ), &taa ) );
    EXPECT_EQ( after.PrevViewProjection, committed.ViewProjection );
    EXPECT_DOUBLE_EQ( after.PrevTimeSeconds, 1.0 );
    EXPECT_EQ( after.JitterIndex, 1u );
}

// ---- RDG-FAULT1 fit -----------------------------------------------------------------------------------------

TEST( TemporalViewContract, VelocityDefaultsToZeroMotionAndHistoryInvalidates )
{
    EXPECT_EQ( kVelocityFaultDefault, RDG::FaultDefault::Black );
    EXPECT_EQ( kHistoryFaultPolicy, RDG::ExternalFaultPolicy::InvalidateHistory );
    EXPECT_EQ( kVelocityFormat, Desert::Core::Formats::ImageFormat::RG16F );
}

TEST( TemporalViewContract, HistoryLostToAFaultResetsTheNextFrame )
{
    SceneViewState        state;
    const FixtureUpscaler taa( TemporalMethod::TAA );
    (void)state.BeginFrame( Inputs( 1.0 ), &taa );

    RDG::Builder                   graph( "TemporalViewContract" );
    const std::vector<HistoryRefs> refs = state.History().Register( graph );
    ASSERT_EQ( refs.size(), 1u );
    EXPECT_TRUE( refs[0].Previous.IsValid() );
    EXPECT_TRUE( refs[0].Current.IsValid() );
    EXPECT_NE( refs[0].Previous, refs[0].Current );

    RDG::ExecuteReport report;
    report.InvalidatedExternals.push_back( refs[0].Current.Index );
    EXPECT_TRUE( state.History().LostToFault( report ) );
    state.EndFrame( report );

    EXPECT_EQ( Value( state.BeginFrame( Inputs( 1.25 ), &taa ) ).HistoryReset, HistoryResetReason::PassFault );
}

TEST( TemporalViewContract, HistoryShapeComesFromTheUpscalerAndResizeRecreatesIt )
{
    SceneViewState        state;
    const FixtureUpscaler taa( TemporalMethod::TAA );
    (void)state.BeginFrame( Inputs( 1.0 ), &taa );
    const uint64_t at1080 = state.History().HeldBytes();
    EXPECT_EQ( at1080, 2ull * 1920 * 1080 * 8 ); // a pair of RGBA16F at OutputExtent
    state.EndFrame( RDG::ExecuteReport{} );

    ViewInputs smaller = Inputs( 1.25 );
    smaller.Output     = { 1280, 720 };
    (void)state.BeginFrame( smaller, &taa );
    EXPECT_EQ( state.History().HeldBytes(), 2ull * 1280 * 720 * 8 );
}

// ---- MotionHistory ------------------------------------------------------------------------------------------

TEST( TemporalViewContract, AnObjectWithNoPreviousFrameHasNoObjectMotion )
{
    MotionHistory   motion;
    const glm::mat4 a = glm::translate( glm::mat4( 1.0f ), glm::vec3( 10, 0, 0 ) );
    const glm::mat4 b = glm::translate( glm::mat4( 1.0f ), glm::vec3( 20, 0, 0 ) );
    EXPECT_EQ( motion.PreviousTransform( { 1, 0 }, a ), a );
    motion.EndFrame();
    EXPECT_EQ( motion.PreviousTransform( { 1, 0 }, b ), a );
    EXPECT_EQ( motion.PreviousTransform( { 1, 0 }, glm::mat4( 3.0f ) ),
               a ); // second record in a frame: first kept
    motion.EndFrame();
    // Not drawn for a frame -> dropped: it reappears without a stale transform.
    motion.EndFrame();
    EXPECT_EQ( motion.PreviousTransform( { 1, 0 }, a ), a );
    EXPECT_EQ( motion.TrackedTransforms(), 0u ); // nothing committed yet this frame
}

TEST( TemporalViewContract, SlotsAreDistinctObjects )
{
    MotionHistory   motion;
    const glm::mat4 a( 2.0f );
    const glm::mat4 b( 4.0f );
    (void)motion.PreviousTransform( { 1, 0 }, a );
    (void)motion.PreviousTransform( { 1, 1 }, b );
    motion.EndFrame();
    EXPECT_EQ( motion.TrackedTransforms(), 2u );
    EXPECT_EQ( motion.PreviousTransform( { 1, 1 }, a ), b );
    EXPECT_EQ( motion.PreviousTransform( { 1, 0 }, b ), a );
}

TEST( TemporalViewContract, BonePaletteOfAnotherSizeIsNotAPreviousPalette )
{
    MotionHistory                motion;
    const std::vector<glm::mat4> three( 3, glm::mat4( 2.0f ) );
    const std::vector<glm::mat4> threeMoved( 3, glm::mat4( 5.0f ) );
    const std::vector<glm::mat4> four( 4, glm::mat4( 7.0f ) );

    const auto first = motion.PreviousBones( { 9, 0 }, three );
    EXPECT_TRUE( std::equal( first.begin(), first.end(), three.begin(), three.end() ) );
    motion.EndFrame();
    const auto second = motion.PreviousBones( { 9, 0 }, threeMoved );
    EXPECT_TRUE( std::equal( second.begin(), second.end(), three.begin(), three.end() ) );
    motion.EndFrame();
    const auto swapped = motion.PreviousBones( { 9, 0 }, four ); // mesh swap
    EXPECT_TRUE( std::equal( swapped.begin(), swapped.end(), four.begin(), four.end() ) );
}

TEST( TemporalViewContract, ASceneChangeClearsMotion )
{
    SceneViewState        state;
    const FixtureUpscaler taa( TemporalMethod::TAA );
    (void)state.BeginFrame( Inputs( 1.0 ), &taa );
    (void)state.Motion().PreviousTransform( { 1, 0 }, glm::mat4( 2.0f ) );
    state.EndFrame( RDG::ExecuteReport{} );
    EXPECT_EQ( state.Motion().TrackedTransforms(), 1u );

    ViewInputs other    = Inputs( 1.25 );
    other.SceneIdentity = 2;
    (void)state.BeginFrame( other, &taa );
    EXPECT_EQ( state.Motion().TrackedTransforms(), 0u );
    EXPECT_EQ( state.Motion().PreviousTransform( { 1, 0 }, glm::mat4( 6.0f ) ), glm::mat4( 6.0f ) );
}

TEST( TemporalViewContract, GpuObjectMotionIsTwoMatrices )
{
    EXPECT_EQ( sizeof( GpuObjectMotion ), 128u );
    EXPECT_EQ( offsetof( GpuObjectMotion, PrevWorld ), 64u );
}
