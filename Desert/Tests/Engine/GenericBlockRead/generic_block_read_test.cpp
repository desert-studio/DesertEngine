// A BLOCK SHORT OF ONE FIELD USED TO COST THE ENTITY THE WHOLE COMPONENT, IN SILENCE.
//
// Eight components in ComponentRegistry.cpp travel as a reflect-cpp mirror struct. The read was
// `rfl::json::read<T>` with no processor, and reflect-cpp refuses a MISSING field even when the struct
// declares a default for it — so `if ( !parsed.has_value() ) return;`, written eight times with no log
// line between them, turned "this file predates one field" into "this entity has no AnimationComponent".
//
// That contradicted the rule ForeignKeys.hpp states and builds its whole preservation argument on: "a
// field ADDED needs no version bump, a missing key already defaults". For these eight it did not
// default, it deleted. Both files described the same load and only one of them was right.
//
// These are the assertions that hold the fix down, on the REAL mirror structs rather than a synthetic
// one — the synthetic version of this test would have passed on the old code just as happily, because
// what makes it real is that `AnimationComponentSer` has six non-optional fields and `Text` has six.

#include <gtest/gtest.h>

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/GenericBlock.hpp>

#include <string>

using Desert::Core::Serialize::ReadBlock;
using Desert::Core::Serialize::WriteBlock;

namespace Assets = Desert::Assets;

namespace
{
    rfl::Generic FromJsonText( const std::string& text )
    {
        auto parsed = rfl::json::read<rfl::Generic>( text );
        EXPECT_TRUE( parsed.has_value() ) << "the test's own fixture is not valid JSON: " << text;
        return parsed.has_value() ? parsed.value() : rfl::Generic( rfl::Generic::Object{} );
    }
} // namespace

// ── THE DEFECT ─────────────────────────────────────────────────────────────────────────────────────
//
// The exact block an older build wrote: `GraphJson` was added after it. Before the fix this returned
// nothing and the caller dropped AnimationComponent off the entity.
TEST( GenericBlockRead, AnAnimationBlockMissingTwoFieldsKeepsTheComponentAndDefaultsThem )
{
    const auto older = FromJsonText( R"({"CurrentClip":"Run","Playing":true,"Loop":false,"PlaybackSpeed":2.5})" );

    const auto parsed = ReadBlock<Assets::AnimationComponentSer>( older, "Animation" );

    ASSERT_TRUE( parsed.has_value() )
         << "one absent field cost the entity its whole AnimationComponent — the character stops being "
            "animated and nothing in the log says why";
    EXPECT_EQ( parsed.value().CurrentClip, "Run" ) << "a field that WAS present did not survive";
    EXPECT_FLOAT_EQ( parsed.value().PlaybackSpeed, 2.5f );
    EXPECT_FALSE( parsed.value().Loop );
    // The absent one takes the struct's own default, which is what every call site already believed.
    EXPECT_TRUE( parsed.value().GraphJson.empty() );
}

TEST( GenericBlockRead, ATextBlockMissingEverythingButItsTextIsStillAText )
{
    const auto older = FromJsonText( R"({"Text":"Press Start"})" );

    const auto parsed = ReadBlock<Assets::TextComponentSer>( older, "Text" );

    ASSERT_TRUE( parsed.has_value() ) << "a text element lost its text because it lacked five other keys";
    EXPECT_EQ( parsed.value().Text, "Press Start" );
    EXPECT_FLOAT_EQ( parsed.value().Size, 1.0f );
    EXPECT_FALSE( parsed.value().Billboard );
}

TEST( GenericBlockRead, AUIAnimBlockMissingItsPlaybackFlagsKeepsItsTracks )
{
    const auto older = FromJsonText( R"({"Tracks":[],"Duration":4.0})" );

    const auto parsed = ReadBlock<Assets::UIAnimComponentSer>( older, "UIAnim" );

    ASSERT_TRUE( parsed.has_value() );
    EXPECT_FLOAT_EQ( parsed.value().Duration, 4.0f );
    EXPECT_FALSE( parsed.value().Loop );
    EXPECT_TRUE( parsed.value().Playing );
}

// ── THE DIRECTION THAT WAS ALREADY SAFE, PINNED SO IT STAYS SAFE ───────────────────────────────────
//
// An older build opening a NEWER build's scene meets keys it has never heard of. That has to keep
// working: it is half of what ForeignKeys.hpp exists to protect, and a processor that tightened this
// direction while loosening the other would be a straight trade rather than a fix.
// `EnableRootMotion` is now one of those keys FOR REAL and not only in a fixture: it was removed from
// AnimationComponentSer when it turned out nothing read it, so every scene written before that carries a
// key this build does not declare. Keeping it in this fixture is the removal's own regression guard.
TEST( GenericBlockRead, AnUnknownExtraFieldDoesNotRefuseTheBlock )
{
    const auto newer =
         FromJsonText( R"({"CurrentClip":"Run","Playing":true,"Loop":true,"PlaybackSpeed":1.0,)"
                       R"("EnableRootMotion":false,"GraphJson":"","SomethingAFutureBuildAdded":7})" );

    const auto parsed = ReadBlock<Assets::AnimationComponentSer>( newer, "Animation" );

    ASSERT_TRUE( parsed.has_value() )
         << "a key this build does not declare made it refuse the whole block, so an older worktree "
            "can no longer open a scene a newer one saved";
    EXPECT_EQ( parsed.value().CurrentClip, "Run" );
}

// ── WHAT MUST STILL BE REFUSED ─────────────────────────────────────────────────────────────────────
//
// `DefaultIfMissing` is not `accept anything`. A field of the wrong type is not a version difference,
// it is a corrupt or hand-broken file, and reading it as a default would be the silent substitution
// the contract forbids. It refuses, and the refusal is logged with the component's key by ReadBlock.
TEST( GenericBlockRead, AFieldOfTheWrongTypeIsStillRefused )
{
    const auto broken = FromJsonText( R"({"CurrentClip":"Run","Playing":"yes","Loop":true,"PlaybackSpeed":1.0,)"
                                      R"("EnableRootMotion":false,"GraphJson":""})" );

    EXPECT_FALSE( ReadBlock<Assets::AnimationComponentSer>( broken, "Animation" ).has_value() )
         << "a string where a boolean belongs was accepted — a malformed block is now indistinguishable "
            "from an old one";
}

// ── AND THE WHOLE TRIP, SO THE TWO HALVES ARE KNOWN TO AGREE ───────────────────────────────────────
TEST( GenericBlockRead, WhatWriteBlockWritesIsWhatReadBlockReads )
{
    Assets::AnimationComponentSer written;
    written.CurrentClip   = "Anim_Idle";
    written.Playing       = false;
    written.Loop          = false;
    written.PlaybackSpeed = 0.25f;
    written.GraphJson     = R"({"nodes":[]})";

    const auto parsed =
         ReadBlock<Assets::AnimationComponentSer>( WriteBlock( written, "Animation" ), "Animation" );

    ASSERT_TRUE( parsed.has_value() );
    EXPECT_EQ( parsed.value().CurrentClip, written.CurrentClip );
    EXPECT_EQ( parsed.value().Playing, written.Playing );
    EXPECT_EQ( parsed.value().Loop, written.Loop );
    EXPECT_FLOAT_EQ( parsed.value().PlaybackSpeed, written.PlaybackSpeed );
    EXPECT_EQ( parsed.value().GraphJson, written.GraphJson );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
