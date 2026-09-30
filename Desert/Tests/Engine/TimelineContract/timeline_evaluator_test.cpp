// THE TIMELINE CONTRACT, groups 3 (the section fold), 5 (the Evaluator and the host seam) and 7 (the TMLN
// format): landed with I6 (Evaluator.cpp) and I4 (SequenceFormat.cpp).

#include "TimelineFixtures.hpp"

#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <format>

using namespace TimelineFixtures;

namespace
{
    float EvaluateOnlyFloat( const Sequence& sequence, FrameTime at )
    {
        const Evaluator evaluator( sequence );
        EvaluatedFrame  frame;
        evaluator.Evaluate( TimeStep{ at, at }, frame );
        EXPECT_EQ( frame.Values.size(), 1U );
        return frame.Values.empty() ? NAN : std::get<float>( frame.Values[0].Value );
    }
} // namespace

// ── 3. Sections, tracks, the sequence: the FOLD (the rest of group 3 is timeline_section_test.cpp) ──

TEST( TimelineSection, FullWeightAbsoluteIsTheKeyValueBitForBit )
{
    FloatChannel channel;
    channel.Default             = 0.1F;
    channel.Keys                = { Key( 0, 0.3F ), Key( 100, 0.7F ) };
    const FloatChannel copy     = channel;
    const Sequence     sequence = OneFloatTrack( channel );
    for ( const FrameTime at : { At( 0 ), At( 33, 0.3F ), At( 100 ) } )
    {
        EXPECT_EQ( EvaluateOnlyFloat( sequence, at ), Evaluate( copy, at, sequence.TickRate ) );
    }
}

TEST( TimelineSection, LaterRowWinsAtFullWeightAndAdditiveZeroIsANoOp )
{
    FloatChannel low;
    low.Keys          = { Key( 0, 1.0F ) };
    Sequence sequence = OneFloatTrack( low );

    Section high = sequence.Tracks[0].Sections[0];
    high.Row     = 1;
    high.Content = Channel{ FloatChannel{ { Key( 0, 5.0F ) }, 0.0F } };
    sequence.Tracks[0].Sections.push_back( high );
    EXPECT_EQ( EvaluateOnlyFloat( sequence, At( 10 ) ), 5.0F );

    Section additive = high;
    additive.Row     = 2;
    additive.Blend   = SectionBlendType::Additive;
    additive.Content = Channel{ FloatChannel{ { Key( 0, 0.0F ) }, 0.0F } };
    sequence.Tracks[0].Sections.push_back( additive );
    EXPECT_EQ( EvaluateOnlyFloat( sequence, At( 10 ) ), 5.0F );
}

TEST( TimelineSection, AHalfWeightAbsoluteIsHalfTheValueFromTheDefault )
{
    FloatChannel channel;
    channel.Default                       = 2.0F;
    channel.Keys                          = { Key( 0, 10.0F ) };
    Sequence sequence                     = OneFloatTrack( channel );
    sequence.Tracks[0].Sections[0].Weight = { Key( 0, 0.5F ) };
    EXPECT_NEAR( EvaluateOnlyFloat( sequence, At( 10 ) ), 6.0F, 1e-6F );
}

// ── 5. The Evaluator and the host seam ──────────────────────────────────────────────────────────────

namespace
{
    struct CountingHost final : ITimelineHost
    {
        int                      Resolves = 0;
        bool                     Present  = true;
        std::vector<std::string> Applied;

        std::optional<ResolvedBinding> Resolve( const Binding& ) override
        {
            ++Resolves;
            return Present ? std::optional<ResolvedBinding>( ResolvedBinding{ 42 } ) : std::nullopt;
        }
        void Apply( const ResolvedBinding&, std::string_view property, const EvaluatedValue& ) override
        {
            Applied.emplace_back( property );
        }
        void Fire( const FiredEvent& ) override
        {
        }
        void SetCamera( const std::optional<ResolvedBinding>& ) override
        {
        }
        void PlayAnimation( const ResolvedBinding&, const AnimationSample& ) override
        {
        }
    };
} // namespace

TEST( TimelineEvaluator, BindingsResolveOncePerRevision )
{
    const Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    Evaluator      evaluator( sequence );
    CountingHost   host;
    EvaluatedFrame frame;
    for ( int i = 0; i < 3; ++i )
    {
        evaluator.Evaluate( TimeStep{ At( i ), At( i + 1 ) }, frame );
        (void)evaluator.Apply( frame, host );
    }
    EXPECT_EQ( host.Resolves, 1 );
    EXPECT_EQ( host.Applied.size(), 3U );
}

TEST( TimelineEvaluator, AnUnresolvedBindingIsReportedByLabelNotSkippedSilently )
{
    const Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    Evaluator      evaluator( sequence );
    CountingHost   host;
    host.Present = false;
    EvaluatedFrame frame;
    evaluator.Evaluate( TimeStep{ At( 0 ), At( 1 ) }, frame );
    const ApplyReport report = evaluator.Apply( frame, host );
    ASSERT_EQ( report.Unresolved.size(), 1U );
    EXPECT_EQ( report.Unresolved[0], "Door" );
    EXPECT_TRUE( host.Applied.empty() );
}

TEST( TimelineEvaluator, AMutedTrackIsAbsentNotDefaulted )
{
    Sequence sequence        = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    sequence.Tracks[0].Muted = true;
    const Evaluator evaluator( sequence );
    EvaluatedFrame  frame;
    evaluator.Evaluate( TimeStep{ At( 0 ), At( 0 ) }, frame );
    EXPECT_TRUE( frame.Values.empty() );
}

// ── 7. Serialization ────────────────────────────────────────────────────────────────────────────────

TEST( TimelineFormat, WriteReadRoundTripsAndATruncatedBlockIsRefused )
{
    const Sequence original = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ), Key( 40, 3.0F ) }, 0.5F } );
    const auto     written  = WriteSequence( original );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    const std::vector<uint8_t>& bytes = written.GetValue();
    const auto                  read  = ReadSequence( bytes );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const Sequence& back = read.GetValue();
    ASSERT_EQ( back.Tracks.size(), 1U );
    EXPECT_EQ( back.Bindings[0].Guid, original.Bindings[0].Guid );
    EXPECT_EQ( back.Tracks[0].Property, "Opacity" );
    for ( const FrameTime at : { At( 0 ), At( 20 ), At( 40 ) } )
    {
        EXPECT_EQ( EvaluateOnlyFloat( back, at ), EvaluateOnlyFloat( original, at ) );
    }

    const std::span<const uint8_t> cut( bytes.data(), bytes.size() / 2 );
    EXPECT_FALSE( ReadSequence( cut ).IsSuccess() );

    // The version is the header's, and an unknown one is refused by name.
    // The file is tampered with as bytes (a newer or older writer's output), not rebuilt: the stated version
    // is found by the header's own tag, so the test never spells the format's JSON.
    const auto statedAs = []( int version )
    {
        return std::format( "\"{}\": {}", Common::Content::FourCCToString( Desert::Assets::kTimelineSchemaTag ),
                            version );
    };
    std::string  text( bytes.begin(), bytes.end() );
    const size_t stated = text.find( statedAs( 2 ) );
    ASSERT_NE( stated, std::string::npos ) << text;
    text.replace( stated, statedAs( 2 ).size(), statedAs( 3 ) );
    const auto newer = ReadSequence( std::vector<uint8_t>( text.begin(), text.end() ) );
    ASSERT_FALSE( newer.IsSuccess() );
    EXPECT_NE( newer.GetError().find( "TMLN v3" ), std::string::npos ) << newer.GetError();

    // v1 (a key's mode shaped the segment ARRIVING at it) has v2's layout and is refused by name all the
    // same: only SceneMigrator reads it (ANIM-FMT).
    text.replace( stated, statedAs( 3 ).size(), statedAs( 1 ) );
    const auto older = ReadSequence( std::vector<uint8_t>( text.begin(), text.end() ) );
    ASSERT_FALSE( older.IsSuccess() );
    EXPECT_NE( older.GetError().find( "TMLN v1" ), std::string::npos ) << older.GetError();
    EXPECT_NE( older.GetError().find( "SceneMigrator" ), std::string::npos ) << older.GetError();
}
