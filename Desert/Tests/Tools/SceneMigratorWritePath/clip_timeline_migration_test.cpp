// THE OLD ANIMATION GENERATIONS, read by the one place allowed to read them: Tools/SceneMigrator.
//
// The engine reads ANIM v6 / TMLN v2 only (AnimationClipFormat proves it refuses the rest by name). Here each
// migrator step is proved on its own input:
//   - TMLN v1 -> v2 in every host: a key's mode moves from the segment ARRIVING at it to the one LEAVING it
//     (UE's rule), proved bit for bit by VerifyInterpShift — a .dseq (the tool's file loop), a scene's UIAnim
//     block (MigrateUIAnimationTimelinesV1ToV2);
//   - generation 3 (ANIM v4/v5, per-bone Channels; SKEL-TREE made v5 the last Channels generation, so there is
//     no ANIM v5 TMLN clip to shift): lifted straight to ANIM v6 / TMLN v2 (MigrateClipGeneration3).
// Generations 0-2 have no reader left anywhere (ANIM-I8a removed MigrateAnimationJson); the engine refuses them.

#include <ClipMigration.hpp>
#include <ClipInterpShift.hpp>
#include <MigratorMain.hpp>
#include <SceneMigration.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs        = std::filesystem;
namespace Migration = Desert::Migration;
namespace Ser       = Desert::Assets::Serialization;
namespace Anim      = Desert::Animation;
namespace Timeline  = Desert::Animation::Timeline;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    const std::string kTmln = Common::Content::FourCCToString( Desert::Assets::kTimelineSchemaTag );
    /// The GUID the restated `.dseq` states: the migrator must carry it through untouched.
    constexpr std::string_view kSequenceGuid = "0000000000000000000000000000d5e1";

    // The modes as authored: key i shapes the segment ARRIVING at it under TMLN v1.
    const std::vector<Anim::KeyInterp> kAuthored = { Anim::KeyInterp::Linear, Anim::KeyInterp::Constant,
                                                     Anim::KeyInterp::Cubic, Anim::KeyInterp::Linear };
    // Under v2 key i takes key i+1's mode; the last keeps its own.
    const std::vector<Anim::KeyInterp> kLeaving = { Anim::KeyInterp::Constant, Anim::KeyInterp::Cubic,
                                                    Anim::KeyInterp::Linear, Anim::KeyInterp::Linear };

    Anim::ScalarKey Key( int32_t tick, float value, Anim::KeyInterp interp )
    {
        Anim::ScalarKey key;
        key.Tick   = Anim::FrameNumber{ tick };
        key.Value  = value;
        key.Interp = interp;
        return key;
    }

    /// One Float track of four keys (the modes above) on the binding kind @p host holds, over [0, 24000].
    Timeline::Sequence FloatSequence( Timeline::SequenceHost host )
    {
        Timeline::Sequence sequence;
        sequence.Host  = host;
        sequence.Start = Anim::FrameNumber{ 0 };
        sequence.End   = Anim::FrameNumber{ 24000 };

        Timeline::Binding binding;
        binding.Guid = Timeline::BindingGuid::Generate();
        binding.Kind = Timeline::BindingKind::Entity;
        if ( host == Timeline::SequenceHost::AnimationClip )
            binding.Kind = Timeline::BindingKind::Sequence;
        else if ( host == Timeline::SequenceHost::UIAnimation )
            binding.Kind = Timeline::BindingKind::Widget;
        if ( binding.Kind != Timeline::BindingKind::Sequence )
            binding.Locator = "Panel";
        binding.Label = "Probe";
        sequence.Bindings.push_back( binding );

        Timeline::FloatChannel     channel;
        const std::array<float, 4> values = { 0.0F, 3.0F, -1.0F, 2.0F };
        for ( std::size_t i = 0; i < kAuthored.size(); ++i )
            channel.Keys.push_back( Key( static_cast<int32_t>( i * 8000 ), values[i], kAuthored[i] ) );

        Timeline::Section section;
        section.Start   = sequence.Start;
        section.End     = sequence.End;
        section.Content = Timeline::Channel{ std::move( channel ) };

        Timeline::Track track;
        track.Binding  = binding.Guid;
        track.Kind     = Timeline::TrackKind::Float;
        track.Property = "Opacity";
        track.Sections.push_back( std::move( section ) );
        sequence.Tracks.push_back( std::move( track ) );
        return sequence;
    }

    /// @p sequence as a TMLN block stating @p version (v1's layout is v2's: only a mode's meaning moved).
    std::string BlockStating( const Timeline::Sequence& sequence, uint32_t version, const char* kind = nullptr )
    {
        const auto written = Timeline::WriteSequence( sequence );
        EXPECT_TRUE( written ) << written.GetError();
        const std::vector<uint8_t>& bytes = written.GetValue();
        auto                        envelope =
             Common::Json::Read<Migration::TimelineEnvelope>( std::string( bytes.begin(), bytes.end() ) );
        EXPECT_TRUE( envelope ) << envelope.GetError();
        Migration::TimelineEnvelope restated = envelope.ExtractValue();
        restated.Header.Versions[kTmln]      = version;
        if ( kind != nullptr )
        {
            restated.Header.Kind = kind;
            restated.Header.Guid = std::string( kSequenceGuid );
        }
        return Common::Json::Write( restated );
    }

    std::vector<Anim::KeyInterp> Modes( const Timeline::Sequence& sequence )
    {
        std::vector<Anim::KeyInterp> modes;
        const auto&                  channel = std::get<Timeline::FloatChannel>(
             std::get<Timeline::Channel>( sequence.Tracks.at( 0 ).Sections.at( 0 ).Content ) );
        modes.reserve( channel.Keys.size() );
        for ( const Anim::ScalarKey& key : channel.Keys )
            modes.push_back( key.Interp );
        return modes;
    }

    Timeline::Sequence ReadCurrent( const std::string& block )
    {
        auto read = Timeline::ReadSequence( std::vector<uint8_t>( block.begin(), block.end() ) );
        EXPECT_TRUE( read ) << read.GetError();
        return read ? read.ExtractValue() : Timeline::Sequence{};
    }

    Anim::AnimationClip EngineClip( const std::string& text )
    {
        const auto read = Ser::ReadAnimationJson( text );
        EXPECT_TRUE( read ) << read.GetError();
        if ( !read )
            return {};
        auto built = Ser::BuildClipFromAssetData( read.GetValue() );
        EXPECT_TRUE( built ) << built.GetError();
        return built ? built.ExtractValue() : Anim::AnimationClip{};
    }

    std::string ReadRaw( const fs::path& p )
    {
        const std::ifstream in( p, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
} // namespace

// ---- TMLN v1 -> v2, the shared step ---------------------------------------------------------------------

TEST( ClipTimelineMigration, AV1BlockIsShiftedToTheLeavingRuleAndProvedBitForBit )
{
    const std::string v1       = BlockStating( FloatSequence( Timeline::SequenceHost::LevelSequence ), 1 );
    const auto        statedV1 = Migration::StatedTimelineVersion( v1 );
    ASSERT_TRUE( statedV1 );
    ASSERT_EQ( statedV1.GetValue(), 1U );

    const auto shifted = Migration::ShiftTimelineV1( v1 );
    ASSERT_TRUE( shifted ) << shifted.GetError();
    EXPECT_EQ( Modes( shifted.GetValue().Shifted ), kLeaving ) << "a mode did not move to the leaving key";
    EXPECT_EQ( shifted.GetValue().KeyLists, 2U ) << "the section's weight curve and its channel";
    EXPECT_GT( shifted.GetValue().SamplesProved, 24000U ) << "every tick of the keyed range must be compared";

    // A v2 block is not a v1 block: the step refuses it rather than shifting it a second time.
    EXPECT_FALSE( Migration::ShiftTimelineV1( BlockStating( FloatSequence( Timeline::SequenceHost::LevelSequence ),
                                                            Timeline::kTimelineFormatVersion ) ) );
}

// ---- .dseq through the tool's own file loop --------------------------------------------------------------

TEST( ClipTimelineMigration, ALevelSequenceFileIsShiftedKeepsItsIdentityAndASecondCheckFindsNothing )
{
    const fs::path dir = fs::temp_directory_path() / "desert_migrator_dseq_v1";
    fs::remove_all( dir );
    fs::create_directories( dir );
    const fs::path file = dir / std::format( "Probe{}", Timeline::kLevelSequenceExtension );
    {
        std::ofstream( file, std::ios::binary )
             << BlockStating( FloatSequence( Timeline::SequenceHost::LevelSequence ), 1, "LevelSequence" );
    }

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ( Migration::RunSceneMigrator( { file.string() }, out, err ), 0 ) << out.str() << err.str();
    EXPECT_NE( out.str().find( "shifted " ), std::string::npos ) << out.str();

    const std::string written = ReadRaw( file );
    const auto        header  = Common::Json::Read<Migration::TimelineEnvelope>( written );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Header.Versions.at( kTmln ), Timeline::kTimelineFormatVersion );
    EXPECT_EQ( header.GetValue().Header.Kind, "LevelSequence" ) << "the .dseq lost its Kind";
    EXPECT_EQ( header.GetValue().Header.Guid, kSequenceGuid ) << "the .dseq lost its GUID";
    EXPECT_EQ( Modes( ReadCurrent( written ) ), kLeaving );

    std::ostringstream again;
    std::ostringstream againErr;
    EXPECT_EQ( Migration::RunSceneMigrator( { "--check", file.string() }, again, againErr ), 0 )
         << again.str() << againErr.str();
    EXPECT_EQ( ReadRaw( file ), written );
    fs::remove_all( dir );
}

// ---- a scene's UIAnim block ------------------------------------------------------------------------------

TEST( ClipTimelineMigration, AUIAnimBlockIsShiftedOnceAndItsOtherMembersStay )
{
    const auto generic =
         rfl::json::read<rfl::Generic>( BlockStating( FloatSequence( Timeline::SequenceHost::UIAnimation ), 1 ) );
    ASSERT_TRUE( generic );
    rfl::Generic::Object block;
    block["Sequence"] = generic.value();
    block["Loop"]     = rfl::Generic( true );
    block["AutoPlay"] = rfl::Generic( false );
    Desert::Assets::EntityData entity;
    entity.Tag                  = "Probe";
    entity.Components["UIAnim"] = rfl::Generic( std::move( block ) );
    std::vector<Desert::Assets::EntityData> entities{ entity };

    const auto report = Migration::MigrateUIAnimationTimelinesV1ToV2( entities );
    EXPECT_TRUE( report.Refused.empty() ) << report.Refused.front();
    EXPECT_EQ( report.Clips, 1U );
    EXPECT_GT( report.SamplesProved, 24000U );

    const rfl::Generic::Object after = entities[0].Components.get( "UIAnim" ).value().to_object().value();
    EXPECT_TRUE( after.get( "Loop" ).value().to_bool().value() );
    EXPECT_FALSE( after.get( "AutoPlay" ).value().to_bool().value() );
    const std::string sequence = rfl::json::write( after.get( "Sequence" ).value() );
    const auto        stated   = Migration::StatedTimelineVersion( sequence );
    ASSERT_TRUE( stated );
    EXPECT_EQ( stated.GetValue(), Timeline::kTimelineFormatVersion );
    EXPECT_EQ( Modes( ReadCurrent( sequence ) ), kLeaving );

    // Keyed on the block's own number: a second pass finds nothing to shift.
    EXPECT_EQ( Migration::MigrateUIAnimationTimelinesV1ToV2( entities ).Clips, 0U );
}

// ---- .anim ----------------------------------------------------------------------------------------------

TEST( ClipTimelineMigration, AGenerationThreeClipLandsOnAnimV6UnderTheLeavingRule )
{
    Migration::ClipGen3::AnimationAssetData gen3;
    gen3.Name          = "Gen3";
    gen3.DurationTicks = 24000;
    gen3.Header        = Common::Content::MakeTextHeader(
         Common::Content::ContentKind::Animation, Common::Content::AssetGuid::Generate(),
         std::array{ Common::Content::SubsystemVersion{ Desert::Assets::kAnimationSchemaTag,
                                                        Ser::kAnimationLastChannelsVersion } } );
    Migration::ClipGen3::ChannelData hips;
    hips.BoneName = "Hips";
    for ( std::size_t i = 0; i < kAuthored.size(); ++i )
    {
        Migration::ClipGen3::KeyPosition key;
        key.Tick         = static_cast<int32_t>( i * 8000 );
        key.Value        = glm::vec3( static_cast<float>( i ), 0.0F, 0.0F );
        key.Shape.Interp = static_cast<int>( kAuthored[i] );
        hips.Positions.push_back( key );
    }
    gen3.Channels.push_back( hips );
    Migration::ClipGen3::EnsureStatedSections( gen3 );

    const auto lifted = Migration::MigrateClipGeneration3( "Gen3.anim", Common::Json::Write( gen3 ), {} );
    ASSERT_TRUE( lifted ) << lifted.GetError();
    const Anim::AnimationClip clip = EngineClip( lifted.GetValue().Text );
    ASSERT_FALSE( clip.Sequence.Tracks.empty() );
    const auto& transform = std::get<Timeline::TransformChannel>(
         std::get<Timeline::Channel>( clip.Sequence.Tracks.at( 0 ).Sections.at( 0 ).Content ) );
    std::vector<Anim::KeyInterp> modes;
    modes.reserve( transform.Translation.X.Keys.size() );
    for ( const Anim::ScalarKey& key : transform.Translation.X.Keys )
        modes.push_back( key.Interp );
    EXPECT_EQ( modes, kLeaving ) << "generation 3 must land on the leaving rule in one step";
}
// NOLINTEND(bugprone-unchecked-optional-access)
