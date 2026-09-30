// Group 9 of the timeline contract (timeline_contract_test.cpp): LiftUIAnimation, the scene v40 -> v41 lift of
// a UI clip (ANIM-I9). The lift is the migration, so these are its acceptance tests: values on the key ticks
// bit for bit, rounding reported, easing reproduced by keys, and every refusal named.

#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Player.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <variant>

namespace
{
    namespace AN = Desert::Animation;
    namespace TL = Desert::Animation::Timeline;

    constexpr int kOffset     = 0; // ECS::UITweenProperty
    constexpr int kOpacity    = 2;
    constexpr int kLinear     = 0; // ECS::UIEasing
    constexpr int kCubicInOut = 6;

    // Records what the evaluator hands the host, by property.
    class RecordingHost final : public TL::ITimelineHost
    {
    public:
        std::map<std::string, TL::EvaluatedValue> Values;

        std::optional<TL::ResolvedBinding> Resolve( const TL::Binding& binding ) override
        {
            if ( binding.Kind != TL::BindingKind::Widget || binding.Locator != "widget-uuid" )
                return std::nullopt;
            return TL::ResolvedBinding{ 1 };
        }
        void Apply( const TL::ResolvedBinding&, std::string_view property, const TL::EvaluatedValue& value ) override
        {
            Values[std::string( property )] = value;
        }
        void Fire( const TL::FiredEvent& ) override
        {
        }
        void SetCamera( const std::optional<TL::ResolvedBinding>& ) override
        {
        }
        void PlayAnimation( const TL::ResolvedBinding&, const TL::AnimationSample& ) override
        {
        }
    };

    RecordingHost EvaluateAt( const TL::Sequence& sequence, const double seconds )
    {
        const AN::FrameTime at = AN::SecondsToFrameTime( seconds, sequence.TickRate );
        TL::Evaluator       evaluator( sequence );
        TL::EvaluatedFrame  frame;
        evaluator.Evaluate( TL::TimeStep{ at, at }, frame );
        RecordingHost host;
        (void)evaluator.Apply( frame, host );
        return host;
    }

    TL::UIAnimationV40 OneTrack( const int property, std::vector<TL::UIAnimationKeyV40> keys )
    {
        TL::UIAnimationV40 v40;
        v40.Duration = 1.0F;
        v40.Tracks.push_back( { property, std::move( keys ) } );
        return v40;
    }

    auto Lift( const TL::UIAnimationV40& v40 )
    {
        return TL::LiftUIAnimation( v40, "widget-uuid", AN::PROJECT_TICK_RATE, AN::DEFAULT_DISPLAY_RATE );
    }
} // namespace

TEST( TimelineLiftUI, KeysOnTheTickGridLiftBitForBitAndReportNoRounding )
{
    const auto v40 = OneTrack( kOffset, { { 0.0F, glm::vec4( 0, 0, 0, 0 ), kLinear },
                                          { 0.5F, glm::vec4( 40, -8, 0, 0 ), kLinear },
                                          { 1.0F, glm::vec4( 100, 3, 0, 0 ), kLinear } } );
    const auto lifted = Lift( v40 );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    const TL::Sequence& sequence = lifted.GetValue().Lifted;
    EXPECT_EQ( sequence.Host, TL::SequenceHost::UIAnimation );
    EXPECT_TRUE( TL::Validate( sequence ).IsSuccess() );
    EXPECT_EQ( lifted.GetValue().Report.RoundedKeys, 0U );

    for ( const auto& key : v40.Tracks[0].Keys )
    {
        const RecordingHost host = EvaluateAt( sequence, key.Time );
        ASSERT_EQ( host.Values.count( "Offset" ), 1U ) << "no Offset at " << key.Time << " s";
        const glm::vec3 v = std::get<glm::vec3>( host.Values.at( "Offset" ) );
        EXPECT_EQ( v.x, key.Value.x ) << "at " << key.Time << " s";
        EXPECT_EQ( v.y, key.Value.y ) << "at " << key.Time << " s";
    }
}

TEST( TimelineLiftUI, AKeyOffTheTickGridIsRoundedAndTheRoundingIsReported )
{
    // A third of a second is 8000 ticks exactly; 0.3333 s is not on the grid.
    const auto lifted = Lift( OneTrack( kOpacity, { { 0.0F, glm::vec4( 1 ), kLinear },
                                                    { 0.3333F, glm::vec4( 0.25F ), kLinear },
                                                    { 1.0F, glm::vec4( 0 ), kLinear } } ) );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    EXPECT_GT( lifted.GetValue().Report.RoundedKeys, 0U ) << "a rounded key went unreported";
    EXPECT_GT( lifted.GetValue().Report.MaxRoundingSeconds, 0.0F );
    EXPECT_LE( lifted.GetValue().Report.MaxRoundingSeconds, 0.5F / 24000.0F + 1e-7F );
}

TEST( TimelineLiftUI, TheEasingOfTheArrivingKeyShapesTheSegment )
{
    // CubicInOut at a quarter of the way is 4 * 0.25^3 = 0.0625 — a straight line would say 0.25.
    const auto lifted = Lift( OneTrack( kOffset, { { 0.0F, glm::vec4( 0 ), kLinear },
                                                   { 1.0F, glm::vec4( 100, 0, 0, 0 ), kCubicInOut } } ) );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    const RecordingHost host = EvaluateAt( lifted.GetValue().Lifted, 0.25 );
    ASSERT_EQ( host.Values.count( "Offset" ), 1U );
    const float x         = std::get<glm::vec3>( host.Values.at( "Offset" ) ).x;
    const float tolerance = 100.0F * lifted.GetValue().Report.MaxEasingDeviation + 0.5F;
    EXPECT_NEAR( x, 6.25F, tolerance ) << "the segment was not eased by its arriving key";
}

TEST( TimelineLiftUI, EveryRefusalIsNamed )
{
    const auto unknownProperty = Lift( OneTrack( 7, { { 0.0F, glm::vec4( 0 ), kLinear } } ) );
    ASSERT_FALSE( unknownProperty.IsSuccess() );
    EXPECT_NE( unknownProperty.GetError().find( "widget-uuid" ), std::string::npos ) << unknownProperty.GetError();

    const auto badEasing = Lift( OneTrack( kOffset, { { 0.0F, glm::vec4( 0 ), 99 } } ) );
    ASSERT_FALSE( badEasing.IsSuccess() );
    EXPECT_NE( badEasing.GetError().find( "99" ), std::string::npos ) << badEasing.GetError();

    const auto unsorted =
         Lift( OneTrack( kOffset, { { 0.5F, glm::vec4( 0 ), kLinear }, { 0.25F, glm::vec4( 1 ), kLinear } } ) );
    EXPECT_FALSE( unsorted.IsSuccess() ) << "keys out of time order were lifted";

    // 1 and 2 hundred-thousandths of a second both round onto tick 0 at 24000 ticks/s.
    const auto collide =
         Lift( OneTrack( kOffset, { { 0.00001F, glm::vec4( 0 ), kLinear }, { 0.00002F, glm::vec4( 1 ), kLinear } } ) );
    EXPECT_FALSE( collide.IsSuccess() ) << "two keys that round onto one tick were lifted";
}
